# État courant du moteur — chiffres historiques, non autoritatifs pour la branche active

> Ce rapport décrit un état antérieur (`arena/1a188065-local-test-share`, base `7374734`) et n'est pas l'état courant de la branche `arena/125b0385-local-test-share`. Depuis, l'architecture a évolué : le transport TLS 1.3/défi-réponse est intégré au plan de contrôle, et le backend WebView2 est explicitement fail-closed mais toujours non opérationnel. Pour l'état vérifié courant, utiliser `docs/reports/IMPLEMENTATION_STATUS.md`; les chiffres ci-dessous restent des preuves historiques uniquement.

Date : **2026-11-14**. Branche de travail : `arena/1a188065-local-test-share`,
base `main` `7374734` (fusion de la PR #4) plus la fusion du commit propriétaire
`82863ed` (`README.md`, `docs/BUILD.md` : commandes Windows préfixées `./`).
Ce document remplace les chiffres des rapports antérieurs **sans les réécrire** :
les rapports datés du 2026-11-13 restent des preuves historiques de leur propre
build.

Vocabulaire imposé, appliqué littéralement dans tout ce document :

- **PASS** : exécuté réellement, résultat conforme, vérifiable par la commande citée.
- **FAIL** : exécuté réellement, résultat non conforme. Rien n'est masqué.
- **SKIP** : non exécuté faute d'environnement (code de sortie 77 en CI). Un SKIP
  n'est **jamais** compté comme un PASS.
- **NON QUALIFIÉ** : exigence du maître qui n'a reçu aucune mesure réelle (deux
  machines, 5 invités, 30 FPS, 720p, p95 < 150 ms, TLS/QUIC, rendu distant).

## Ce qui a été mesuré, et avec quoi

| Profil | Commande réelle | Résultat |
| --- | --- | --- |
| Unitaire, `-O0 -Wall -Wextra -Werror` | `plpt-unit-tests` | **PASS — 33 cas / 20 882 vérifications / 0 échec** |
| Intégration LAN, `-O0` | `plpt-platform-tests` | **PASS — 12 cas / 622 vérifications / 0 échec** |
| ASan + UBSan, `-O1`, fuites activées | les deux binaires | **PASS — mêmes totaux, 0 échec**, aucun rapport de sanitizer |
| `_GLIBCXX_ASSERTIONS`, `-O0` (profil de débogage Windows) | les deux binaires | **PASS — mêmes totaux, 0 échec**, aucune assertion libstdc++ |
| Stabilité du nouveau cas de saturation | `plpt-platform-tests` ×3 en `-O0 -D_GLIBCXX_ASSERTIONS`, ×2 en `-O2`, puis ×2 après factorisation du client HTTP des tests | **PASS — 12 cas / 622 vérifications / 0 échec à chaque exécution** |
| Collision du macro Windows `interface` | compilation de **toutes** les sources produit et test avec `-Dinterface=struct` (simulation exacte de `<iphlpapi.h>`) | **PASS — aucune erreur**, sans `#undef` ni `push_macro` |
| Service de démonstration, exécution réelle | `.cache/demo_diag.sh` (HTTP réel, 26 connexions maintenues ouvertes, `SIGTERM`) | **PASS — voir le détail ci-dessous** |
| WebSocket RFC 6455 du service de démonstration | client jetable `/tmp/smoke/ws_client.cpp` | **PASS — `101`, écho texte exact, ping→pong, close** |
| Énumération des interfaces, exécution réelle | `resolve_bind_address` | **PASS** — `lo` 127.0.0.1 / ::1 et `eth0` 169.254.0.21 / fe80::fc:ff:fe00:5 %2 ; eth0 retenue, repli lien local signalé |
| Mode `--ipv6` des tests plateforme | `plpt-platform-tests --ipv6` | **SKIP — code 77** : aucun IPv6 utilisable dans cet environnement |
| **Qualification Windows, chaîne imposée** (clang++ MSYS2 UCRT64, `build.bat`) | CI run [`43607807540`](https://github.com/MONST-CODE/Local-test-share/actions/runs/43607807540), job `Native Windows UCRT64 / owner clang++` | **PASS — 11 min 42 s, 17 étapes en succès** : `deps --fetch`, `doctor`, `ide-check`, Debug (`cmd.exe /d /c build.bat debug`), tests Debug + fenêtre native, Release, tests Release + fenêtre native, `selftest`, `clean`, `package` et exécution sans MSYS2 dans le `PATH`. Le run précédent (`43600395896`) était **FAIL** sur la collision `interface` |
| **Qualification Linux, chaîne imposée** (clang++ natif, `build.sh`) | CI run [`43607807540`](https://github.com/MONST-CODE/Local-test-share/actions/runs/43607807540), job `Native Linux x64 / direct clang++` | **PASS — 19 min 12 s, 17 étapes en succès** : `deps --fetch`, `doctor`, **ASan + UBSan** (`xvfb-run -a ./build.sh test asan --native-ui`), Debug depuis un checkout frais, tests Debug + fenêtre X11, Release, tests Release + fenêtre X11, **libFuzzer**, `selftest`, `clean`, `package --native-ui` |
| ASan UCRT64 et libFuzzer Windows | CI, mêmes étapes | **SKIP (2 étapes `skipped`)** : runtime indisponible sur le job Windows, sortie 77 attendue par la CI. Jamais compté comme un PASS ; la couverture sanitizer réelle est celle du job Linux (clang++ + ASan/UBSan) et des profils locaux ci-dessus |
| Fuzzing Linux (libFuzzer, clang++ natif) | CI, étape `libFuzzer smoke` | **PASS** |

**Chaîne de compilation de ces mesures :** `g++ 12.2.0` sur Debian 12, en tant
qu'outil de vérification intermédiaire uniquement (aucun `clang` n'est installé dans
cet environnement et `PLPT_CXX` n'a jamais été pointé sur `g++`). La **qualification**
appartient à la CI : `clang++` natif Linux et `clang++` MSYS2 UCRT64 natif Windows, via
`build.sh` et `build.bat`.

**Qualification obtenue pour cet incrément :** run CI
[`43607807540`](https://github.com/MONST-CODE/Local-test-share/actions/runs/43607807540)
du 2026-11-14 (15:00:33Z → 15:19:49Z), **`success` sur les deux jobs**, sur les
commits `1e8ed68` (fusion de `main`) et `b33f4a1` (corrections). C'est ce run — pas la
compilation locale — qui qualifie les deux chaînes imposées. La CI ne déclenche que sur
`pull_request` et `workflow_dispatch` : un push de branche seul ne lance rien.

### Détail du diagnostic du service de démonstration (PASS, mesuré)

Capacité annoncée par le code : **8 connexions actives + 16 tâches en attente**, soit
24 au total, et rien d'autre — le même `engine/platform/connection_pool.hpp` que celui
exercé par les tests d'intégration.

| Observation | Valeur mesurée |
| --- | --- |
| Threads du processus, au repos / sous 8 requêtes parallèles / pendant la saturation / après libération | **9 / 9 / 9 / 9** (1 principal + 8 ouvriers, jamais plus) |
| 5 requêtes séquentielles, puis 8 parallèles | `200` ×5, `200` ×8 |
| 26 connexions TCP **réelles maintenues ouvertes sans requête** | capacité saturée : 8 actives + 16 en attente, 2 refusées |
| Requête pendant la saturation | **`503` en 14 ms**, corps « service de démonstration saturé » |
| Requête après libération des connexions | **`200`** : aucune place n'est perdue |
| 30 requêtes parallèles après saturation | `200` ×30 |
| `/ws` sans upgrade | **`426 Upgrade Required`** |
| `SIGTERM` | **sortie 0 en 200 ms**, bilan écrit : `69 connexion(s) prise(s) en charge et 4 refusée(s) : actives_pic=8 attente_pic=16 terminées=69` |
| `stderr` du service | **vide** |

Les pics `actives_pic=8` et `attente_pic=16` sont exactement les bornes configurées :
elles n'ont jamais été dépassées, et la ligne de bilan n'est écrite qu'après le
`join()` de tous les ouvriers — c'est la preuve que l'arrêt est propre, pas celle
d'un processus tué.

## Corrections de cet incrément de stabilisation (PR #5)

1. **Collision du macro Microsoft `interface`** (`#define interface struct`, posé par
   `<iphlpapi.h>`, atteint par `-liphlpapi`) : 3 déclarations de `socket.hpp`,
   3 définitions et 12 usages de `socket.cpp`, 1 variable de boucle de
   `discovery.cpp` renommés en `local_interface` / `lan_interface`. Les champs
   Winsock `imr_interface` / `ipv6mr_interface` ne sont pas touchés (le macro ne
   remplace que le jeton exact). Aucun `#undef` : le macro reste intact pour le code
   Microsoft. C'était la cause du **FAIL** Windows du run `43600395896`.
2. **Fusion propre de `main`** : commit de fusion `1e8ed68` intégrant `82863ed`
   (documentation des commandes Windows). Aucun conflit, aucune modification du
   propriétaire réécrite, **aucun push forcé**.
3. **Modèle de capacité du service de démonstration** : les connexions **actives** et
   les tâches **en attente** sont désormais deux notions distinctes, observables
   séparément (`active()`, `queued()`, pics `max_active`/`max_queued`), avec une
   capacité totale unique et cohérente `workers + queue_capacity`. Au-delà, la
   soumission est **refusée** et l'appelant reste propriétaire de la socket : il
   répond `503` puis ferme. Un `Refused` ne consomme jamais la socket.
4. **Test automatisé de saturation avec connexions maintenues ouvertes** :
   `pool_saturation_with_held_open_connections` ouvre 6 connexions TCP réelles sur un
   pool de 2 ouvriers + 2 places d'attente, vérifie `Active` ×2, `Queued` ×2,
   `Refused` ×2, qu'aucune tâche ne se termine pendant que les connexions sont tenues
   (`completed == 0`), qu'une soumission supplémentaire est refusée tant que rien ne
   se libère, puis la bascule `completed == 2 && active == 2 && queued == 0` après
   libération, et enfin `stop()` : tous les ouvriers joints, plus aucune soumission
   acceptée.
5. **La place d'ouvrier est réservée dès la soumission** (`Task::reserved`) : sans
   cela, deux soumissions rapprochées pouvaient être toutes deux classées « en
   attente » alors qu'aucun ouvrier n'avait encore été réveillé par l'ordonnanceur.
   Défaut révélé par le profil `_GLIBCXX_ASSERTIONS -O0` (2 échecs), corrigé, puis
   5 exécutions consécutives sans échec.
6. **Réponses informationnelles et upgrade dans le proxy** : `100 Continue`,
   `103 Early Hints` et tout `1xx` sauf `101` sont transmis **tels quels**, et
   l'interception continue sur les octets suivants (plusieurs `1xx` peuvent arriver
   dans le même segment TCP : la boucle les traite tous, bornée par les octets reçus).
   Le cookie de capacité n'est déposé que dans une réponse **pertinente** (`2xx`–`5xx`).
   Sur `101` ou sur une réponse illisible, la réponse n'est **jamais modifiée** et le
   renoncement est compté (`grants_skipped`) et averti (`ProtocolViolation`).
7. **Octets réels jamais perdus à la fermeture de l'amont** : un en-tête de réponse
   incomplet au moment où le service local ferme est transmis à l'invité, et l'échec du
   dépôt est compté (`grants_failed`) et averti. Défaut trouvé par le nouveau test :
   sans cette boucle, seul le `100 Continue` atteignait l'invité.
8. **Arrêt du service de démonstration réellement propre** — et **correction d'une
   affirmation fausse de ce même document** : la version précédente écrivait
   « arrêt en 100 ms après `SIGTERM` » alors qu'**aucun gestionnaire de signal n'était
   installé**. Les 100 ms mesurés étaient la terminaison par défaut du processus : les
   ouvriers n'étaient **pas** joints et la ligne de bilan n'était **pas** écrite.
   Désormais `sigaction` sans `SA_RESTART` sous POSIX (sinon `accept()`/`receive()`
   bloquants seraient relancés et l'arrêt ne serait jamais observé), `std::signal`
   sous Windows, écoute **non bloquante** pour que la boucle voie l'arrêt demandé sur
   les deux plateformes, et délais bornés par connexion (`10 s` pour une requête muette,
   `120 s` d'inactivité WebSocket alignés sur `idle_timeout_ms` du proxy) : un client
   muet ne peut plus occuper un ouvrier indéfiniment.

### Le cookie de capacité est-il « délivré » ? Preuve exigée et fournie

Le cookie n'est considéré comme délivré **que si son insertion dans une réponse
pertinente est démontrée**. `lan_proxy_informational_and_upgrade_responses` le vérifie
octet par octet sur TCP réel en boucle locale :

- `100 Continue` + `103 Early Hints` + `200 OK` : la réponse du client commence par
  `HTTP/1.1 100 Continue`, l'en-tête `Link:` du `103` est présent, **aucun
  `Set-Cookie:` avant la ligne `HTTP/1.1 200 OK`**, et
  `Set-Cookie: plpt=<jeton>; Path=/; HttpOnly; SameSite=Strict` apparaît **après**
  cette ligne ; compteurs `informational_responses == 2`, `grants_issued == 1`,
  `grants_skipped == 0`, `grants_failed == 0`, `tokens_redacted == 1`.
- Upgrade WebSocket réel : `101 Switching Protocols` et `Sec-WebSocket-Accept`
  traversent **sans aucune modification**, `Set-Cookie:` est absent, et le service
  local reçoit `GET /ws HTTP/1.1` avec `Sec-WebSocket-Key` **sans jamais voir le
  jeton**.
- `101` sur une requête **sans** upgrade : transmis tel quel, `grants_skipped == 1`,
  `grants_issued == 0`, avertissement présent.
- Réponse amont illisible (`PAS-UNE-REPONSE`) : transmise telle quelle, aucun cookie,
  `grants_skipped == 1`, avertissement présent.

## Statuts par domaine

| Domaine | Statut | Preuve / réserve |
| --- | --- | --- |
| Détection native (processus, ports, arbre, classification) | **IMPLÉMENTÉ, TESTÉ (PASS)** | cas unitaires et plateforme ; Linux `proc`, adaptateurs Windows |
| Registre de services et sélection | **IMPLÉMENTÉ, TESTÉ (PASS)** ; non relié au partage | observation seulement, jamais une autorisation |
| Allocation de port (port 0, socket conservée, fds rendus) | **IMPLÉMENTÉ, TESTÉ (PASS)** | `ipv4_allocation`, `repeated_allocation_releases_fds` |
| Énumération d'interfaces et adresse de liaison | **IMPLÉMENTÉ, TESTÉ (PASS)** | ADR-014 ; instantané au démarrage, pas de basculement en session |
| Pool de connexions à capacité explicite | **IMPLÉMENTÉ, TESTÉ (PASS)** | `pool_saturation_with_held_open_connections` + diagnostic du service réel |
| Plan de contrôle hôte/invité (protocole 1.2) | **IMPLÉMENTÉ, INTÉGRÉ, TESTÉ (PASS)** en boucle locale | TCP **en clair** : non qualifié pour une session réelle |
| Identité et reprise de place | **IMPLÉMENTÉ, TESTÉ (PASS)** | secret aléatoire rotaté ; ADR-014 |
| Messagerie (général, DM, quotas, rédaction) | **IMPLÉMENTÉ, TESTÉ (PASS)** | présence, réactions, mentions, modération : à faire |
| Proxy de service (capacité retirée, cookie de capacité, `1xx`, `101`) | **IMPLÉMENTÉ, TESTÉ (PASS)** en boucle locale | HTTP clair, pas de HTTP/2 ni de demi-fermeture ; ADR-013 |
| Découverte LAN (multicast, broadcast, saisie manuelle) | **IMPLÉMENTÉ, TESTÉ (PASS)** en boucle locale | multicast inter-machines **NON QUALIFIÉ** ; mDNS/DNS-SD non évalué |
| IPv6 | **SKIP (77)** | aucun IPv6 utilisable dans l'environnement de test ; jamais compté PASS |
| Browser Worker, capture, encodage, décodage, rendu distant | **ABSENT** | `NullBrowserBackend` échoue explicitement ; aucun faux rendu |
| Interface SDL3/ImGui branchée sur le réseau | **NON IMPLÉMENTÉ** | le moniteur local fonctionne ; aucune session ouverte depuis l'UI |
| Orchestration (détection → registre → allocation → serveur → proxy → worker) | **NON IMPLÉMENTÉ** | aucun enchaînement câblé |
| Nettoyage et profils navigateur temporaires | **PARTIEL** | fermeture `Closing→Cleanup→Closed` prouvée ; stockage temporaire indisponible, explicitement |
| Transport sécurisé (TLS/QUIC) | **NON IMPLÉMENTÉ** | ADR de transport à instruire ; **aucune fonctionnalité LAN sécurisée n'est déclarée** ; aucun repli silencieux ajouté |
| Charge 5 invités / 30 FPS / 720p / p95 < 150 ms | **NON QUALIFIÉ** | aucune mesure revendiquée |
| LAN physique deux machines | **NON QUALIFIÉ** | une seule machine disponible : procédure propriétaire à écrire, critères à marquer NON QUALIFIÉS |
| Arrêt par `Ctrl+C` du service sous Windows | **NON QUALIFIÉ** | même code que sous Linux, jamais exécuté sur une machine Windows ici |

Qualification par les deux chaînes imposées : le run CI
[`43607807540`](https://github.com/MONST-CODE/Local-test-share/actions/runs/43607807540)
est **`success`** sur les deux jobs (Windows clang++ MSYS2 UCRT64 en 11 min 42 s,
Linux clang++ natif en 19 min 12 s, Debug **et** Release, tests directs et fenêtre
native/X11, ASan+UBSan et libFuzzer côté Linux). Les lignes marquées **TESTÉ (PASS)**
le sont donc aussi avec `clang++` et non seulement avec `g++`. Les lignes **SKIP** et
**NON QUALIFIÉ** le restent : la CI ne lève ni l'absence d'IPv6, ni l'indisponibilité
du runtime ASan/libFuzzer sous Windows, ni l'absence de deuxième machine.

## Défauts corrigés dans l'incrément précédent (rappel, tous testés)

1. Fuite du jeton de capacité vers le service local (ADR-013).
2. Sous-ressources d'une page autorisée refusées en `403` (ADR-013).
3. Réattribution d'une place sur simple homonymie (ADR-014).
4. Écoute sur toutes les interfaces par défaut, sans énumération réelle possible (ADR-014).
5. Threads du service de démonstration jamais joints, `detach()` à la sortie, et
   saturation définitive après 8 connexions.
6. Purge des demandes en attente par couple (nom, adresse), ambiguë.

## Reste ouvert, par ordre de risque

1. **Browser Worker / capture / encodage / décodage** : le différenciateur fondamental
   du produit est absent. Prototype WebView2 sous clang++ UCRT64 et porte de
   validation (DPI 100/150/200 %, fenêtre minimisée ou occluse, entrées, fermeture,
   suppression des données temporaires) avant toute interface définitive.
2. **Transport sécurisé** : comparaison de piles QUIC/TLS maintenues compatibles avec
   les deux chaînes imposées ; aucune réimplémentation de TLS/QUIC ; aucune session
   réelle en TCP clair.
3. **Interface branchée** sur `host_server`/`guest_client` via instantanés immuables
   et commandes sur files bornées.
4. **Orchestration** et cycle de vie complet (§9 du maître), récupération après crash.
5. **Qualification LAN physique** et charge mesurée.
