# ADR-017 — Authentification par défi-réponse et basculement du plan de contrôle sur TLS 1.3

**Statut :** accepté (18 novembre 2026)
**Contexte :** mandat §24 et §9.2 (transport sécurisé réel, aucune crypto
maison), étape 5 du plan de reconstruction. ADR-016 a livré le transport
versionné (interface v2 : descripteur, non-bloquant, `wrap_server`/`wrap_client`)
et la fabrique TLS 1.3 à PSK externe, mais **rien n'était branché** : l'hôte et
l'invité parlaient encore TCP clair en production.

Deux faits d'audit ont imposé cette décision plutôt qu'un simple branchement :

1. Dans le protocole 1.2, `GuestHello` portait **le code d'invitation en clair**
   (`engine/net/guest_client.cpp`, champ `invite_code`) et le **secret de
   reprise en clair** lors d'une reconnexion. Un observateur du LAN capturait
   donc le secret d'admission, puis pouvait rejoindre la session.
2. La première ébauche du basculement (commit `74322c3`) faisait porter à la
   notice `TlsUpgrade` un secret tiré par l'hôte, **transmis en clair** juste
   avant la poignée de main. Un PSK livré en clair ne protège contre rien :
   l'observateur qui lit la notice déchiffre ensuite tout le canal. Annoncer
   « TLS 1.3 » dans cet état aurait été un état sécurisé fictif — exactement ce
   que le mandat interdit.

Autrement dit : brancher TLS sans changer la distribution du secret aurait
produit un chiffrement de théâtre.

## Décision

### 1. Révision 1.3 : le secret se prouve, il ne se transmet pas

Trois types de trames forment la porte d'entrée (`engine/protocol/envelope.hpp`) :

| Type | Sens | Statut | Contenu |
|---|---|---|---|
| `AuthChallenge = 17` | hôte → invité | clair | nonce public de 32 octets (entropie OS) |
| `AuthProof = 18` | invité → hôte | clair | preuve HMAC du code et/ou du secret de reprise |
| `TlsUpgrade = 16` | hôte → invité | **dernière trame claire** | identité PSK publique, nature du secret, nonce du défi |

Ces trames voyagent avant qu'une place existe : `validate_header`
(`engine/protocol/envelope.cpp`) n'autorise un identifiant de participant nul
que pour `Hello`, `Auth`, `AuthChallenge`, `AuthProof` et `TlsUpgrade`. Toute
autre trame exige une place attribuée. La séquence reste obligatoire (sauf
`Hello`) : ni le défi, ni la preuve, ni la notice ne sont rejouables.

### 2. Preuve et dérivation : HMAC-SHA256, aucun algorithme maison

`engine/core/mac.{hpp,cpp}` implémente HMAC (RFC 2104) au-dessus du fournisseur
d'empreintes de l'OS déjà utilisé par `engine/core/digest.hpp` (CNG sous
Windows, AF_ALG puis libcrypto chargé dynamiquement sous Linux). La
construction est vérifiée contre les **vecteurs RFC 4231** 1, 2, 3, 6, 7 et le
cas clé vide/message vide (contrôle indépendant : `hmac` de Python).

- preuve : `HMAC(secret, "DevLink auth proof v1|" || nonce || session_be64 || nature)`
- secret de transport : `hex(HMAC(secret, "DevLink TLS13 PSK v2|" || nonce || session_be64 || identité))`

où `secret` est soit les octets décodés du code d'invitation (80 bits,
`kInviteCodeBytes`), soit les octets du secret de reprise (128 bits,
`kResumeSecretBytes`). Les deux côtés dérivent **indépendamment** le même secret
de transport ; il ne traverse jamais le fil. Ce secret est passé à la fabrique
ADR-016 comme PSK externe, qui produit TLS 1.3 en `TLS_AES_128_GCM_SHA256` avec
authentification mutuelle par la poignée de main.

La nature du secret (`AuthProofKind::InviteCode` / `ResumeSecret`) sépare les
domaines : une preuve de code n'est pas une preuve de reprise, et inversement.

### 3. Le chemin clair historique est BLOQUÉ, pas ignoré

Un client qui enverrait encore `invite_code` ou `resume_secret` dans `Hello`
est refusé net (`ErrorCode::ProtocolViolation`, « secret transmis en clair
refusé »). De même, l'invité refuse une admission qui n'a pas été précédée d'un
upgrade quand `tls_required` est vrai. Un chemin non sûr n'est jamais laissé
fonctionnel « au cas où ».

### 4. Ordre des opérations : chiffrer avant toute trame utile

```
Hello (clair, aucun secret)
  → AuthChallenge (clair, nonce)
  → AuthProof (clair, HMAC)
  → vérification à temps constant, compteurs et limites de tentatives inchangés
  → TlsUpgrade (dernière trame claire, aucun secret)
  → poignée de main TLS 1.3 (descripteur cédé, délai borné, fd non bloquant après)
  → Auth (secret de reprise), SessionState, ServiceList, Chat, Heartbeat : CHIFFRÉS
  → attente d'approbation éventuelle : chiffrée aussi
```

L'upgrade précède l'approbation : même l'attente d'une décision de l'hôte est
couverte. Un échec de poignée de main déconnecte — jamais de repli clair
silencieux.

### 5. Disponibilité honnête et refus de démarrer

- `HostConfig.tls_required` (défaut **true**) : sans runtime TLS, `start()`
  échoue `ErrorCode::Unsupported` avec la raison exacte du runtime, et aucun
  port n'est ouvert. L'interface affiche l'erreur au lieu d'annoncer une
  session.
- `tls_required = false` des **deux** côtés : le clair est assumé et
  **étiqueté** partout — `HostSnapshot.transport`, `ParticipantRecord.transport`,
  `GuestSnapshot.transport`, `GuestPeer.transport`, `SessionInfo.transport`
  (bloc 1.3 optionnel du codec, absent en 1.2), pastilles de l'interface.
  Aucune de ces étiquettes n'est devinée : elles viennent de `TransportInfo`
  (protocole et suite réellement négociés) ou du constat d'indisponibilité.

### 6. Non-bloquant et tampons TLS

Après l'upgrade, tout passe par `ITransportChannel` (interface v2) : envois
partiels, `ErrorCode::WouldBlock`, `descriptor()` pour le `poll`. Un segment TCP
peut porter plusieurs enregistrements TLS déjà déchiffrés et conservés dans le
canal : `poll` ne les signalera plus. L'hôte vidait déjà sa lecture
(`kMaxReceivePerPoll`) ; la boucle d'authentification de l'invité a reçu le même
drainage, sans quoi `Auth` serait traité sans `SessionState` ni `ServiceList`.
La poignée de main remet le descripteur en **bloquant** le temps de l'échange
(délais socket temporaires bornés) puis le passe en non-bloquant : le fd adopté
peut venir d'un pair déjà non-bloquant.

### 7. Suppression du canal maison

`engine/net/secure_channel.{hpp,cpp}` (XOR + HMAC, magie `0x444C4E4B`) est
supprimé du dépôt avec `MIGRATION_SOURCES` dans `build.sh` et `build.bat`, son
inclusion dans `HEADERS`, et le cas de test de migration. La décision ADR-016
prévoyait cette suppression « jusqu'au basculement » : le basculement est fait,
la crypto maison ne reste pas dans l'arbre.

## Options écartées

- **Notice PSK en clair** (ébauche `74322c3`) : chiffrement de théâtre face à un
  observateur passif. Rejetée.
- **Certificat auto-signé sans épinglage** : aucun ancrage de confiance,
  homme-du-milieu trivial. Rejeté.
- **PAKE (SPAKE2/OPAQUE)** : le bon outil pour un secret faible, mais exige une
  implémentation cryptographique supplémentaire ; hors du périmètre « aucune
  crypto maison » tant que la pile ne le fournit pas. Le code d'invitation
  reste à 80 bits avec limites de tentatives en ligne (voir Risques).
- **Attendre une PKI interne** : repousse le mandat sans rien livrer.

## Risques et limites assumés

1. **Absence de confidentialité persistante** : le PSK dérive d'un secret à
   durée de vie longue (code de session). Une capture déchiffrable plus tard si
   le code fuit. Mitigation : le nonce lie chaque session, le secret de reprise
   tourne à chaque admission, et le code n'est plus jamais sur le fil.
2. **Code dictable à 80 bits** : cassage hors ligne impossible (HMAC, nonce
   unique), devinette en ligne limitée par `kMaxCodeFailuresPerConnection` et la
   limite par adresse déjà en place. Un PAKE resterait plus robuste : à
   instruire si le mandat l'exige.
3. **Fenêtre claire initiale** : `Hello`, `AuthChallenge`, `AuthProof` et la
   notice voyagent en clair. Ils ne contiennent aucun secret ; un attaquant
   actif peut couper la connexion (déni de service), pas la lire ni s'y
   substituer.
4. **Windows** : la fabrique TLS s'y déclare indisponible tant que le runtime
   n'est pas câblé ; le comportement est alors le refus honnête de démarrer,
   jamais un clair silencieux. Statut IMPLEMENTED_BUT_NOT_VERIFIED pour le
   chemin Windows, comme en ADR-016.

## Preuves exigées (et où elles vivent)

| Propriété | Preuve exécutée |
|---|---|
| HMAC exact | `hmac_sha256_matches_rfc4231` (vecteurs RFC 4231 + clé vide) |
| Codec et bornes du défi/preuve/notice | `challenge_response_codec_and_derivation` (allers-retours, troncatures, octets surnuméraires, tailles hors borne, séparations de domaine, dérivations liées au contexte, trames avant admission) |
| Le chemin clair est bloqué | `control_plane_challenge_response_gates_admission` : `Hello` avec code en clair → refus `ProtocolViolation`, aucun participant |
| Le défi existe réellement | idem : `AuthChallenge` reçu, nonce 32 o, session cohérente, charge utile exactement `4 + 32` octets |
| Preuve forgée refusée et comptée | idem : `Auth` refusé `Unauthorized`, `counters.bad_codes` nourri, aucune admission, aucun upgrade |
| Preuve exacte → canal chiffré avant `Auth` | idem : `TlsUpgrade` avant tout `Auth` lisible, puis plus aucune trame claire décodable tandis que l'hôte admet le pair |
| Aucun secret sur le fil | `lan_control_wire_never_carries_secrets` : relais octet-par-octet entre invité et hôte ; ni le code (texte **et** octets), ni le secret de reprise, ni les messages, ni les libellés de dérivation n'apparaissent dans la capture |
| Étiquettes réelles des deux côtés | `lan_control_plane_host_and_guest`, `lan_resume_secret_binds_identity` |
| Refus sans runtime TLS | `host_refuses_session_without_tls_runtime` + auto-test `tls-host-refusal-proof` (`DEVLINK_TLS_LIBRARY_PATH` introuvable) : `start()` échoue `Unsupported`, port 0, et le clair assumé est étiqueté |

SKIP n'est pas PASS : les cas qui exigent le runtime TLS ne sont enregistrés que
dans la branche où il est présent ; la branche inverse exerce le refus.
