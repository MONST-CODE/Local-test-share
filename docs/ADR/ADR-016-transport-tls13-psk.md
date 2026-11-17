# ADR-016 — Transport des sessions réelles : interface versionnée, TLS 1.3 PSK authentifié, disponibilité honnête

**Statut :** accepté (17 novembre 2026)
**Contexte :** mandat §24 et §9.2 (transport sécurisé des sessions), rubrique
« à instruire » du registre des ADR (comparaison de piles TLS maintenues,
compatible clang++ UCRT64 et clang++ Linux). Au commit `1e12c08`, le seul
canal « sécurisé » du dépôt était `engine/net/secure_channel.cpp` : XOR de la
charge utile avec un keystream dérivé du secret de reprise, en-tête à HMAC
SHA-256 anti-rejeu, magie `0x444C4E4B`. Crypto maison : confidentiel mais
**malléable** (un attaquant actif peut altérer le clair), aucune authentification
de pair par poignée de main, aucune confidentialité persistante si le secret
fuit, et rien de maintenu par un tiers. Le mandat exige une pile TLS 1.3
maintenue avec authentification du pair.

## Décision

1. **Interface de transport versionnée** (`engine/net/transport.hpp`,
   `kTransportInterfaceVersion = 1`, incrémenté à toute rupture) :
   - `ITransportChannel` : `send`/`receive`/`close`/`info` ;
   - `ITransportEndpoint` : `accept`/`port`/`close` ;
   - `ITransportFactory` : `listen`/`connect` **et** `available()` /
     `unavailability_reason()` — une fabrique dont le runtime manque le dit
     explicitement au lieu d'échouer plus tard sans diagnostic ;
   - `TransportInfo{protocol, cipher, peer_authenticated, interface_version}` :
     **tout** canal doit se déclarer. Le TCP clair déclare `"tcp clair"` et
     `peer_authenticated=false` — jamais un canal non sécurisé ne peut
     prétendre l'être (mandat : pas d'état « sécurisé » fantaisiste).

2. **Deux fabriques, pas de repli implicite :**
   - `make_clear_tcp_factory()` : pour le local/test, toujours auto-déclaré clair ;
   - `make_tls13_factory(policy)` : politique strictement TLS 1.3 par défaut
     (`min = max = 0x0304`) ; une politique incohérente (`min > max`) ou hors
     bornes est **refusée à la construction**, jamais corrigée en silence.

3. **TLS 1.3 réel à PSK externe, authentification mutuelle :**
   - identité + secret partagé ≥ 16 octets, échangés hors bande (le secret de
     session du plan de contrôle ADR-014 joue ce rôle, éphémère par conception) ;
   - clé PSK = SHA-256(`"DevLink TLS13 PSK v1|identity|secret"`) via
     `EVP_Q_digest` — libellé versionné pour permettre une évolution explicite ;
   - la poignée de main prouve **des deux côtés** la connaissance du secret :
     `peer_authenticated=true` ne sort que d'une poignée aboutie ;
   - suite unique épinglée des deux côtés : `TLS_AES_128_GCM_SHA256`
     (`SSL_CTX_set_ciphersuites`). Sans cet épinglage, OpenSSL préfère
     `TLS_AES_256_GCM_SHA384`, dont le digest rend la session PSK SHA-256
     incompatible : le serveur la **jette** et retombe sur une poignée de main à
     certificats (`no suitable signature algorithm`) — cause racine vérifiée
     contre les sources `openssl-3.0.20` (`tls_parse_ctos_psk`,
     `tls_choose_sigalg`) puis corrigée et re-testée ici ;
   - échec de poignée ⇒ `ErrorCode::Unauthorized`, **aucun objet canal** rendu,
     aucun repli sur du clair, message d'erreur portant la raison OpenSSL réelle.

4. **Chargement runtime sans dépendance de build (Linux) :** OpenSSL est chargé
   par `dlopen` (`libssl.so.3` puis `libssl.so` ; `libcrypto` idem), symboles
   liés un à un, sans en-têtes OpenSSL à la compilation (le sandbox Debian 12
   de développement n'en fournit pas ; c'est aussi le cas de nombreuses
   toolchains cibles). Détails vérifiés contre les sources 3.0.20 :
   `SSL_CTX_set_min/max_proto_version` sont des **macros** — on lie
   `SSL_CTX_ctrl` avec les constantes publiées 123/124 ;
   `SSL_get_cipher_name` est une macro — on lie `SSL_get_current_cipher` +
   `SSL_CIPHER_get_name` ; `SSL_SESSION_set_master_key` s'appelle en réalité
   `SSL_SESSION_set1_master_key`. Runtime absent ou symbole manquant ⇒
   `available()=false` + raison explicite ; `listen`/`connect` échouent
   `ErrorCode::Unsupported`. Variable de test `DEVLINK_TLS_LIBRARY_PATH`
   (chemin imposé au dlopen) pour prouver la branche « indisponible » en
   auto-test — elle ne peut que faire échouer, jamais faire croire.

5. **Windows : honestement non câblé à ce stade.** La fabrique TLS y rapporte
   `available()=false` avec la raison « runtime TLS non câblé sur Windows
   (Schannel/OpenSSL MSYS2 : ADR-016) ». L'interface, la politique, la
   dérivation PSK et les tests sont prêts ; le câblage Schannel (natif, zéro
   dépendance) ou OpenSSL MSYS2 est un suivi documenté. Statut :
   IMPLEMENTED_BUT_NOT_VERIFIED depuis Linux — aucun état « sécurisé » n'est
   prétendu sur Windows tant que la poignée n'y est pas exécutée et vérifiée.

6. **Basculement différé (étape 5) :** `transport.cpp`/`transport_tls.cpp`
   (`TRANSPORT_SOURCES`) sont liés **aux tests uniquement** — même régime que
   `secure_channel.cpp` (`MIGRATION_SOURCES`) — tant que `host_server` /
   `guest_client` ne les consomment pas. Aucun binaire livré n'emporte du TLS
   inutilisé ; le jour du basculement, le canal XOR/HMAC cesse d'être lié et
   l'interface versionnée permet de refuser proprement un pair ancien
   (`kTransportInterfaceVersion`).

## Conséquences

- Les tests d'intégration prouvent le transport **réel** sur loopback :
  poignée aboutie des deux côtés avec le bon secret, refus des deux côtés avec
  un mauvais secret ou une version inférieure, et oracle de fil :
  un relais octet-par-octet enregistre le trafic — le marqueur applicatif n'y
  apparaît **jamais** en clair (chiffrement effectif, pas seulement déclaré).
- `secure_channel.cpp` reste lié aux tests de migration jusqu'à l'étape 5, qui
  le retirera des sources liées et marquera la rupture `v2` du protocole de
  migration comme obsolète (les pairs `0x444C4E4B` ne seront plus acceptés).
- Le coût runtime d'OpenSSL n'existe que là où TLS est réellement utilisé ;
  le TCP clair reste disponible sans aucune bibliothèque externe.

## Risques et parades

- **Surface ABI du dlopen** : chaque symbole est vérifié à la liaison, tout
  manque produit une raison explicite et `Unsupported` — jamais de repli muet.
- **PSK sans PFS contre enregistrement passif** : accepté en connaissance de
  cause — le secret est celui d'une session de 30 s (ADR-014), pas un mot de
  passe persistant ; la rotation vient du plan de contrôle, pas du transport.
- **DoS pendant poignée** : borné par le pool de connexions existant (8+16) et
  par l'absence de certificats à négocier ; le serveur ne rend aucun canal
  avant la fin de la poignée.
- **Dérive OpenSSL 4.x** : les constantes `SSL_CTX_ctrl` 123/124 et les noms de
  symboles sont documentés ici avec leur version de vérification (3.0.20) ;
  un changement casserait le dlopen, donc `available()=false` + raison — échec
  honnête, pas corruption silencieuse.

## Tests (vérifiés sur cette sandbox Linux, 17 novembre 2026)

`tests/integration/platform_tests.cpp` :
- `transport_clear_declares_itself` — le clair se déclare `"tcp clair"` /
  `peer_authenticated=false` / cipher vide, des deux côtés, et round-trip ;
- `tls13_psk_loopback_authenticated` — `info()` = `TLSv1.3`,
  `TLS_AES_128_GCM_SHA256`, authentifié client **et** serveur ; marqueur
  applicatif échangé ;
- `tls13_psk_wrong_secret_refused` — mauvais secret : refus des deux côtés,
  `Unauthorized`, aucun canal ;
- `tls13_downgrade_refused` — politique inversée refusée à la fabrique ;
  client TLS 1.2-only refusé par le serveur 1.3 strict, des deux côtés ;
- `tls13_wire_oracle_no_plaintext` — relais octet-par-octet : > 200 octets
  capturés, marqueur jamais en clair, libellé de dérivation jamais sur le fil ;
- `tls13_availability_is_honest` — cohérence `tls13_runtime_available()` /
  `available()` / raison ; avec `DEVLINK_TLS_LIBRARY_PATH=/does/not/exist`
  (auto-test `tls-unavailable-proof` de `build.sh`) : indisponibilité explicite,
  `Unsupported` sur listen, et les cas TLS affichent **SKIP** — SKIP ≠ PASS,
  la suite reste verte et honnête (14 cas au lieu de 18, aucun échec masqué).

Résultat : `platform: 18 cases, 670 checks, 0 failures` (runtime présent) ;
`SELFTEST_EXIT=0` dont `PASS tls-unavailable-proof` (runtime forcé absent).
