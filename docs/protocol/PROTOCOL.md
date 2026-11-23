# Protocole DevLink — plan de contrôle, messagerie et canal média

**Version courante : 1.5** (`kProtocolMinor`, `engine/protocol/envelope.hpp`).
Historique des mineures : 1.1 partage LAN, 1.2 reprise de place, 1.3 upgrade TLS
(ADR-016/017), 1.4 offre média (ADR-018), 1.5 signalements et identifiant de
l'hôte (ADR-019). Les mineures sont rétrocompatibles au décodage ; l'hôte refuse
explicitement un invité de mineure inférieure.
Ce document remplace les anciennes affirmations « version 1.0 sans handshake » de
la version historique de ce fichier. Le codec teste la syntaxe ; il ne suffit pas à
autoriser une opération réseau.

## Enveloppe de contrôle

Les entiers sont sérialisés en ordre réseau big-endian ; aucune structure C++ native
n'est copiée telle quelle. L'en-tête fixe fait 56 octets :

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 4 | magic `PLPT` (`50 4c 50 54`) |
| 4 | 2 | version majeure (1) |
| 6 | 2 | version mineure (courante 5) |
| 8 | 2 | type de message |
| 10 | 2 | réservé, doit être zéro |
| 12 | 8 | session ID |
| 20 | 8 | participant ID |
| 28 | 4 | stream ID |
| 32 | 8 | séquence |
| 40 | 8 | horodatage monotone |
| 48 | 4 | longueur payload |
| 52 | 4 | flags (actuellement zéro) |
| 56 | variable | payload typé et borné selon le message |

Le décodeur vérifie la longueur exacte, les champs, la version et la limite du type
avant de copier le payload. Les plafonds incluent notamment 256 KiB pour le type
`VideoFrame`, mais ce plafond de codec n'est pas une implémentation de flux vidéo.

## Admission et protection du plan de contrôle

Le code du plan de contrôle a ajouté les types 16–18 : `TlsUpgrade`,
`AuthChallenge` et `AuthProof`. Le parcours visé/implémenté dans le moteur est :

1. Hello public sans code d'invitation ni secret de reprise transmis en clair ;
2. nonce aléatoire de l'hôte ;
3. preuve HMAC de possession du code ou du secret de reprise, liée au nonce et à
   la session ;
4. notice d'upgrade sans secret ;
5. poignée TLS 1.3 PSK avant l'admission et les échanges applicatifs.

TLS 1.3 PSK est fourni par OpenSSL 3 (système sous Linux ; DLL UCRT64 épinglées
chargées depuis le dossier de l'application sous Windows). Les preuves sont en
boucle locale : ni test physique LAN, ni audit cryptographique indépendant. Voir `engine/net/transport*`, `engine/net/wire.*`,
`docs/ADR/ADR-016-transport-tls13-psk.md`, `docs/ADR/ADR-017-defi-reponse-et-basculement-tls13.md`
et la preuve datée de basculement.

## Messages et limites

Les types de base couvrent Hello/Auth, état/contrôle, entrée, frame réservée,
chat, présence, feedback, heartbeat et erreurs. Les types 13–18 ajoutent l'état de
session, liste de services, départ, upgrade TLS, défi et preuve. Les tailles sont
centralisées dans `max_payload_size()` ; l'encodeur/décodeur versionné et les
payloads typés se trouvent dans `engine/net/wire.*`.

Ces vérifications ne prouvent pas à elles seules l'identité de l'appelant, son rôle,
sa capacité ou son droit à une entrée. Ces décisions doivent être vérifiées par le
moteur de session et attachées au canal réellement authentifié.

## Messagerie et signalements (protocole 1.5, ADR-019)

Tous ces messages circulent sur le canal de contrôle **déjà basculé en TLS 1.3**.
L'identité d'expéditeur vient toujours de la connexion authentifiée, jamais du
payload.

- **Chat** (`Chat`, 8) : `ChatText` = expéditeur, nom, corps UTF-8 borné, horodatage,
  drapeau système, destinataire (`0` = canal général, sinon message privé). L'hôte
  relaie selon le modèle `Messaging` (quotas, visibilité) ; l'auteur reçoit l'écho
  de ce que l'hôte a accepté, messages privés compris.
- **Identifiant de l'hôte** : bloc optionnel 1.5 de `SessionInfo` (`SessionState`,
  13), placé après le bloc 1.3 : `u64` non nul, distinct de tout pair listé. Il
  permet à un invité d'écrire en privé à l'hôte. Ce n'est pas un secret.
- **Signalement** (`Feedback`, 10), invité → hôte seulement : `FeedbackReport` =
  `u8` gravité (0 information, 1 mineure, 2 majeure, 3 bloquante), `u64`
  horodatage d'envoi, description UTF-8 de 1 à 2 048 octets, contexte UTF-8 de
  256 octets au plus **sans caractère de contrôle**. Toute borne violée, gravité
  inconnue, troncature ou octet surnuméraire fait refuser le payload. L'hôte
  attribue l'identifiant (`ReportId`), réhorodate, conserve en mémoire (200 au
  plus, oubliés à l'arrêt) et envoie un accusé système **à l'auteur seul**. Le
  journal d'événements ne contient jamais le texte. Ni capture, ni pièce jointe.

## Codec média et canal média (protocole 1.4, ADR-018)

`engine/browser/remote_frame.hpp` définit maintenant un codec de payload versionné
à entiers big-endian, avec magic/header explicites, identité de session/participant,
stream/contexte, séquence, timestamp monotone, dimensions, stride et taille déclarée.
La version 1 accepte uniquement des frames complètes RGBA8 sans compression, jusqu'à
4096×2160 et avec une taille exacte `stride × height`. Les inputs ont un codec
versionné distinct, coordonnées normalisées Q0.16, champs réservés contrôlés et
texte UTF-8 borné à 32 octets. Des tests négatifs et une entrée fuzz exercent ces
parseurs ; ils n'établissent pas une connexion de production.

Le codec v2 ajoute le type JPEG (images du compositeur WebView2), validé contre
les dimensions SOF avant décodage.

**Canal média** (`engine/share/media_wire.*`), une connexion TCP par invité,
séparée du contrôle :

1. Après admission, et seulement sur un contrôle TLS, l'hôte envoie
   `MessageType::MediaOffer` (19) : port, identité PSK publique
   `devlink-media-s<session>-p<participant>`, clé de 32 octets propre au
   participant, mode, viewport. Il n'y a jamais d'offre en clair.
2. L'invité envoie un unique `Hello` en clair (`u32 longueur`, `u8 1`,
   `u32 "DLMH"`, `u16 version=1`, `u64 session`, `u64 participant`). Tout octet
   supplémentaire avant TLS fait refuser la connexion.
3. TLS 1.3 PSK avec la clé du participant (TLS 1.2 refusé), puis des enregistrements
   `u32 longueur big-endian | u8 type | corps`, longueur bornée **avant**
   allocation : `Frame` (2 : `u64 last_input_seq` + image v2), `Input` (3 :
   `u64 séquence > 0` + événement), `Control` (4 : 1 = demander, 2 = rendre le
   contrôle), `State` (5 : mode, vous, contrôleur, époque du bail, drapeaux,
   viewport, dernier refus, note), `Closing` (6).

L'hôte n'injecte un `Input` qu'après `SharePolicy::authorize` (participant non
révoqué, séquence strictement croissante, non suspendu, mode, bail). Un
enregistrement hors bornes ferme le canal. Les modes A/B/C, les attaques (mauvaise
clé, usurpation, autre session, downgrade, rejeu, enregistrement surdimensionné)
et la charge à 5 invités sont testés dans `tests/integration/share_pipeline_tests.cpp`.
Le parcours complet entre les vrais exécutables l'est dans `desktop_e2e_tests.cpp`.

## Tests existants

Unitaires (`tests/unit/test_main.cpp`) : encodages et limites de chaque payload,
mutations d'enveloppe, négociation de version dérivée de `kProtocolMinor`,
défi-réponse, codec média, signalement et bloc 1.5. Intégration
(`tests/integration/platform_tests.cpp`) : plan de contrôle réel en boucle locale,
TLS 1.3, mauvais secret, refus sans runtime, chat, messages privés (invité ↔ invité,
invité → hôte, hôte → invité) et signalements. Partage et média :
`share_pipeline_tests.cpp`, `desktop_e2e_tests.cpp` (Windows). État vérifié :
[`docs/reports/IMPLEMENTATION_STATUS.md`](../reports/IMPLEMENTATION_STATUS.md).
