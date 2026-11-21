# NOT_FINALIZED — DevLink, audit d'implémentation et qualification

## A. Résumé exécutif

Date selon la session : **2026-11-13 UTC**. Les commandes exécutées dans ce
sandbox horodatent néanmoins leurs sorties au **2026-11-19 UTC** (la plus récente
preuve de cette continuation se termine à 19:50:30Z). Cette divergence est conservée
explicitement : les timestamps bruts ne sont ni modifiés ni réinterprétés comme une
chronologie certifiée. Les rapports suivis portant des dates du 14–18 novembre sont
également conservés comme historiques, sans les antidater.

**Conclusion : `NOT_FINALIZED` — reconstruction en cours, non prête à être
annoncée ni distribuée comme partage réel.** Le contrôle hôte/invité, le transport
TLS 1.3 PSK côté Linux, le poste desktop, le branding et les scripts de build ont
beaucoup avancé. L'élément différenciateur du produit — WebView2 réel, capture de
pixels, transport média, affichage invité et interactions — demeure absent. La
qualification Windows, le LAN physique et le DPI multi-moniteurs restent ouverts.

- Branche : `arena/125b0385-local-test-share`.
- HEAD : `1a70c6600c5253433badd9ce446087242c62de5d`, tip actuel de
  `arena/125b0385-local-test-share` et de `origin/arena/125b0385-local-test-share`.
  `main`/`origin/main` : `1e12c08b2137808b65a50564b35f29a8dfac4971`. Les cinq commits
  distants préexistants sont `99a08fb`, `d4708c4`, `74322c3`, `a79a37e`, `1a70c66`.
  Local a été fast-forwardé sans réécriture jusqu'au tip après préservation du worktree.
- Le worktree contient les changements préservés et les incréments media ajoutés
  dans cette exécution ; ils restent non commités. Le test GCC exploratoire post-
  changement a été relancé (résultats en §E) ; il ne remplace pas le build officiel
  Clang. L'empreinte exacte du patch courant est enregistrée dans le manifeste.
- `stash@{0}` (`preserve-worktree-before-fast-forward-to-active-remote-branch`)
  est conservé comme sauvegarde supplémentaire ; ne pas le supprimer.
- Sauvegarde pré-implémentation de ce mandat sous
  `.cache/mandate-20261113-start/` : diff indexé/non-indexé, tar des non-suivis et
  manifeste SHA-256. La sauvegarde précédente `.cache/pre-execution-backup/` est
  également conservée.
- Le mandat complet fourni par l'utilisateur dans la conversation est la référence
  d'acceptation. Le fichier nommé MASTER_PROMPT n'est pas versionné dans le checkout.
- Aucun commit, push, PR ou merge n'a été effectué dans cette continuation. Vérification GitHub : aucun PR, ouvert ou fermé, n'est associé à ce head/branche (`gh pr list --state all --head ...` retourne `[]`).
- Aucune release partage fonctionnel n'est présente dans `releases/`. Le manifeste
  de qualification `docs/reports/RELEASE_MANIFEST.json` déclare `NOT_FINALIZED`,
  référence les preuves disponibles par SHA-256 et n'invente aucun artefact ni
  attestation de release.
- Les rapports suivis `BUILD_VALIDATION_LINUX.md`, `BUILD_VALIDATION_WINDOWS.md` et
  `UCRT64_WINDOWS_VALIDATION.md` contiennent des résultats PASS détaillés, mais
  leurs identifiants correspondent à d'autres branches/commits. À titre de contrôle,
  `gh run view 43607807540` retourne le run `success` du 14 novembre sur la branche
  `arena/1a188065-local-test-share`, HEAD `b33f4a1a...`, pas sur cette branche ni sur
  son commit de base. Ces artefacts sont des preuves historiques de leur source
  propre, pas une qualification du diff audité. Leurs dates sont aussi postérieures
  à la date de session. Aucun WebView2/pipeline média/LAN physique n'en découle.

Le rapport n'est pas un audit de sécurité indépendant. Les résultats GCC sont des
qualifications locales exploratoires, pas une qualification Clang++ UCRT64.

## B. Préservation Git et état de travail

Avant les modifications de cette continuation : `git status --short --branch`,
`git branch --show-current`, `git log -5 --oneline --decorate`, `git diff --stat` et
la liste des fichiers non suivis ont été relevés. Beaucoup de changements suivis et
non suivis étaient déjà présents. Ils ont été sauvegardés sans les écraser :

- `.cache/pre-execution-backup/tracked.diff` — patch binaire de `git diff HEAD` ;
- `.cache/pre-execution-backup/untracked.tar.gz` — archive des fichiers non suivis ;
- `.cache/pre-execution-backup/backup-manifest.txt` — branche, HEAD et SHA-256.

La sauvegarde est dans `.cache/` (ignoré et exclu des snapshots), pas dans Git.
SHA-256 enregistrés : patch `43f5acd80f05ce618a89f57a422970b4a6a255961f3f1254619b6ebb9f5572da` ;
archive non suivis `b1cb290ac11fca305dd984e13543cd5ee81942bf606f1aab69643a8ea6b8e29a`.

Modifications de cette continuation : restauration des sondes de ressources PE/ICO
et des fixtures synthétiques manquantes, correction de l'appel `renamed-ico`, de
l'appel attendu par `build.bat selftest`, correction du contrôle de provenance
UCRT64 dans le probe, restauration de la sonde DPI et du convertisseur BMP→PNG,
rafraîchissement de plusieurs guides et de ce rapport. Les modifications locales préexistantes sont préservées dans le worktree/stash et
n'ont pas été supprimées. Après découverte du tip distant `1a70c66`, l'état local a
été sauvegardé dans `stash@{0}` et sous `.cache/mandate-20261113-start/`, puis la
branche active a été fast-forwardée (aucun changement de branche ou réécriture).
L'application du stash a produit des conflits ; ils ont été résolus en prenant le
contenu sauvegardé pour les fichiers sources/docs en conflit, puis les preuves
préexistantes du stash restent conservées. Aucun reset, clean destructif, push, PR
ou merge GitHub n'a eu lieu.

## C. Constats d'audit du checkout réel

1. **Fichiers exigés par les scripts absents du checkout à l'inspection.**
   `build.sh` et `build.bat` invoquaient les options `--ico`, `--pe-icons` et
   `--manifest --expect-version`, mais `tests/build/toolchain_probe.cpp` était encore
   l'ancien probe limité à `--manifest <path>` et aucun `resource_probe.hpp` ou
   `synthetic_pe.py` n'existait. Le selftest ne pouvait pas démontrer ses contrôles
   annoncés. Les sources manquantes sont maintenant versionnées dans le workspace,
   avec parseurs bornés et fixtures générées.
2. **Provenance UCRT64 incohérente.** `build.bat` définit
   `DEVLINK_MSYS2_ROOT`, alors que le probe attendait seulement
   `PLPT_MSYS2_ROOT`. Le probe accepte maintenant le nom DEVLINK courant, avec
   fallback historique PLPT. Cette correction compile sous GCC Linux mais ne peut
   pas être validée en UCRT64 ici.
3. **Échec intentionnel trompeur du selftest Windows.** `build.bat` utilisait
   `:expect` avec un code de sortie volontairement inattendu (23), ce qui affichait
   `FAIL` même si le selftest attendait ce code. Le test vérifie directement code
   et sortie, comme le script Linux.
4. **Procédure DPI non reproductible.** Les PNG existaient, mais la sonde et le
   convertisseur cités dans le rapport n'étaient pas dans le checkout. Ils ont été
   recréés, recompilés strictement avec les véritables objets UI/ImGui/SDL3, exécutés
   et les captures ont été régénérées. Le facteur reste simulé, pas physique.
5. **Documentation périmée et contradictoire.** README, guide utilisateur,
   installation, réseau, protocole, sécurité et release notes faisaient des
   affirmations qui ne correspondaient plus au code (partage réel annoncé, TLS
   décrit comme totalement absent ou authentification présentée comme terminée).
   Ces documents ont été révisés ou annotés ; des rapports datés restent explicitement
   historiques.
6. `engine/media/` était absent dans l'audit de départ ; cette exécution y ajoute
   `frame_queue.hpp`. `engine/browser/remote_frame.hpp` a été remplacé par un codec
   de payload média/input v1 explicite et borné : champs
   big-endian, identités/sequence/timestamp, RGBA8 complet, dimensions/stride/taille
   validés, Q0.16 pour les coordonnées, et texte UTF-8 borné. Les parseurs sont
   exercés par les tests unitaires et un harness aléatoire ASan/UBSan. Ce codec n'est
   pas encore relié à l'enveloppe TLS, à WebView2, à un renderer invité ou à une
   autorisation d'input. `engine/browser/webview2_backend.cpp` échoue toujours fermé ;
   aucun partage réel n'est revendiqué.
7. Les étapes 6 et 7 du plan précédent sont identifiées dans le mandat fourni :
   **6 — partage applicatif réel** (navigateur, capture/transport/rendu, interactions,
   modes et proxy) ; **7 — séparation démonstration/tests/production**. La barrière
   fail-closed et le mode synthétique explicite couvrent seulement une partie de 7 ;
   l'étape 6 reste non réalisée.
8. **Défaut allowlist corrigé dans cette continuation.** `BrowserPolicy` comparait
   auparavant des préfixes de chaînes et acceptait `*`, ce qui permettait par exemple
   à `http://localhost.evil/...` de correspondre à `http://localhost`. La politique
   délègue maintenant à `engine/browser/url_origin.hpp` : HTTP(S) ASCII seulement,
   canonicalisation du schéma/hôte/port, origine exacte, pas d'userinfo/joker, rejet
   des IP IPv4 non standard et normalisation IPv6. Tests unitaires dédiés ajoutés.
   Cette fonction reste une primitive non branchée sur un WebView2 (absent), donc
   elle ne prouve pas le filtrage de redirections/sous-ressources.
9. Le backend synthétique a été renommé `DemoBrowserBackend`, sa fabrique
   `create_demo_browser_backend()`, et son emploi dans l'UI reste conditionné au
   flag explicite `--demo-synthetic-browser`. Il continue à produire volontairement
   des pixels fictifs et ne doit pas être pris pour un moteur de production.
10. **Régression selftest découverte après synchronisation de branche.** Le tip
    distant contenait une ancienne `tests/build/synthetic_pe.py` dont l'interface CLI
    n'acceptait pas le `--out-dir` appelé par le `build.sh` courant. Le selftest a
    échoué exit 2 ; le log est `continuation-synthetic-cli-regression.log`. La version
    déterministe compatible (`--out-dir`, 4 fixtures positives/négatives) sauvegardée
   dans le stash a été restaurée. Le selftest suivant passe toutes les assertions PE,
   TLS et publication ; preuve `continuation-synced-gcc-selftest.log`.
11. **Incrément média/input portable ajouté à cette session.** `remote_frame.hpp`
    sérialise une frame RGBA8 v1 avec header big-endian explicite et champs de
    session/participant/stream/contexte/séquence/timestamp/dimensions/stride/longueur ;
    le décodeur borne avant allocation et rejette toute incohérence. Le codec input
    v1 utilise coordonnées Q0.16, types/flags/réservés validés et UTF-8 limité.
    `engine/media/frame_queue.hpp` fournit une file bornée en nombre/octets qui
    abandonne les frames complètes les plus anciennes, rejette les séquences
    réordonnées/contextes différents et permet un drain après fermeture. Cela reste
    non connecté à WebView2, à TLS/média transporté ou au rendu invité.
12. **Portabilité Clang stricte réparée sans baisser les warnings.** Le Clang test
    exposait un avertissement `std::stable_sort` déprécié de libstdc++ avec `-Werror`;
    `HostServer::build_chat()` utilise une insertion stable bornée à 200 messages.
    Les champs WebView2 stubs inutilisés ont été supprimés au lieu d'ajouter des
    annotations de suppression. Test Clang Debug/Release et selftest officiels passent.

## D. Matrice des exigences et preuves

Les statuts utilisent exactement la nomenclature du mandat. « GCC local » signifie
le build/test lancé via une copie temporaire de `build.sh` dans `.cache/` dont la
sélection de compilateur a été adaptée à GCC. Ce n'est pas le script officiel ni une
qualification Windows.

| ID / exigences | Statut | Résultat / preuve actuelle | Action restante |
|---|---|---|---|
| GOV-01 — sauvegarder le travail suivi et non suivi | `PARTIAL` | manifest de sauvegarde `.cache/pre-execution-backup/` et hashes ci-dessus | Conserver la sauvegarde jusqu'à décision utilisateur |
| GOV-02 — branche active et absence d'opérations Git interdites | `PARTIAL` | branche confirmée ; aucun commit/push/PR/merge/reset/clean | Aucune sans autorisation |
| AUD-01 — lire le rapport de continuation et le mandat fourni | `PARTIAL` | rapport complet lu ; texte maître reçu dans la conversation | Le texte maître n'est pas enregistré dans le dépôt |
| AUD-02 — matrice de conformité | `PARTIAL` | matrice par domaines dans ce rapport ; le détail des parcours physiques impossibles reste ouvert | Enrichir au fil des preuves Windows/E2E |
| BUILD-01 — scripts officiels disponibles/documentés | `IMPLEMENTED_NOT_VERIFIED` | `./build.sh help` passe ; `build.bat` audité statiquement | Exécuter les deux sur leurs plateformes natives |
| BUILD-02 — doctor/build Linux Clang++ >=18 | `PARTIAL` | `./build.sh doctor --core-only` exit 0, recompte dans `continuation-official-clang-doctor-clean-resources.log`. Clang 23.1.5 et resource headers épinglés par intégrité npm, uniquement dans `.cache/`; wrapper final n'injecte aucun chemin d'en-têtes GCC et aucun flag warning n'est retiré. Doctor/tests Linux passent, mais cette provenance n'est ni le paquet système du runner CI ni la chaîne Windows UCRT64 | Reproduire sur runner natif CI et attacher au SHA commité |
| BUILD-03 — tests Linux Clang/GCC | `PARTIAL` | test officiel `build.sh` Clang Debug et Release, headless : 39 cas/21 193 checks unit, 20/760 platform, 2/41 IPv6 ; 15 réussis, 0 ignoré, exit 0. Logs `continuation-official-clang-test-debug-final.log` et `continuation-official-clang-test-release-final.log`. GCC exploratoire aussi exécuté ; aucun SHA commité | Rejouer sur SHA commité, puis native-ui X11 et qualification package |
| BUILD-04 — selftest Linux Clang/GCC | `PARTIAL` | `./build.sh selftest` Clang exit 0 : fixtures ICO/PE, refus TLS-runtime absent, publication atomique de fixture et clean ; `continuation-official-clang-selftest-final.log`. GCC selftest antérieur également conservé | Rejouer sur SHA commité et qualifier l'archive produit extraite |
| BUILD-05 — Clang++ MSYS2 UCRT64/Windows | `BLOCKED_BY_ENVIRONMENT` | le checkout actuel n'a pas été qualifié sous Windows dans cette continuation ; les rapports suivis citent des succès UCRT64 sur d'autres commits/branches (voir §A) | Exécuter/attacher les résultats au HEAD final de cette branche sur Windows x64 natif |
| BUILD-06 — sources/ressources des probes de build | `PARTIAL` | CLI du probe, `resource_probe.hpp`, fixture PE `--out-dir` compatibles restaurés ; selftest GCC passé. Header `url_origin.hpp` listé dans les sondes build Linux/Windows | Vérifier avec Clang Windows/Linux et ajouter un vrai contrôle source-manifest |
| UI-01 — interface SDL3/ImGui, smoke | `PARTIAL` | UI smoke dummy/software dans tests Clang Debug et Release ; partage affiché désactivé, `NOT RUN` fenêtre X11 native | Smoke natif Windows et Linux X11 |
| UI-02 — actions UI reflètent un moteur réel | `PARTIAL` | transactions et diagnostics sont en place, mais certains parcours de partage sont fermés/incomplets | Parcours hôte/invité réel après Browser Worker |
| BRAND-01 — master SVG et exports | `PARTIAL` | `python3 tools/branding/export_icons.py --verify` exécuté dans cette continuation : `VERIFICATION: OK`, 7 ICO entries et PNG hashes affichés | Ressources inspectées dans les vrais PE Windows |
| BRAND-02 — DPI typographique logiciel | `PARTIAL` | sonde réelle ImGui/SDL3, 4 captures, tailles/pixels/géométrie, `lines_fit=1` | Aucun énoncé de DPI physique fondé sur cette sonde |
| BRAND-03 — PerMonitorV2 physique / icône taskbar PE | `BLOCKED_BY_ENVIRONMENT` | aucun Windows/moniteur physique ; fixtures PE seulement | Tester fenêtre, taskbar, icônes et déplacement 100–200 % sur Windows |
| SESSION-01 — cycle contrôle hôte/invité | `PARTIAL` | moteur/protocole tests loopback ; pas de parcours produit complet avec navigateur | E2E réel, erreurs et nettoyage |
| SESSION-02 — Modes A/B/C et permissions côté moteur | `PARTIAL` | A = test indépendant/profil distinct ; B = collaboration sur une vue/bail transférable ; C = observation seule/sans entrée. Le modèle serveur rôle/capacité et tests de session existent, mais pas d'événement WebView2 réel ni de client réseau adversarial qualifié | E2E adversarial : rejet de toute entrée en C, bail B, indépendance A et révocation |
| SEC-01 — défi-réponse/anti-rejeu contrôle | `PARTIAL` | boucle de contrôle défi/réponse validée dans platform tests GCC (`control_plane_challenge_response_gates_admission`) ; aucune qualification LAN physique | Revue complète de dérivation/identité et test physique |
| SEC-02 — TLS 1.3 PSK Linux | `PARTIAL` | code OpenSSL réel ; tests loopback, mauvais secret/downgrade/refus-runtime passent dans les suites Clang 23 et GCC ; vérification non indépendante | Clang CI reproductible, durcissement et revue externe avant contexte sensible |
| SEC-03 — TLS Windows/UCRT64 | `BLOCKED_BY_ENVIRONMENT` | fabrique Windows indisponible/non câblée | Choisir/brancher une pile maintenue et tester natif |
| SEC-04 — confidentialité de tous les plans | `PARTIAL` | plan de contrôle TLS Linux ; proxy applicatif et futur média non prouvés chiffrés | Authentifier/protéger chaque canal et vérifier absence de clair |
| PROXY-01 — proxy et autorisation | `PARTIAL` | proxy HTTP et capacités testés loopback ; pas de qualification LAN sécurisée | SSRF, ports, redirections, TLS, révocation et ressource amont |
| BROWSER-01 — WebView2 environnement/contrôleur/profil | `NOT_IMPLEMENTED` | `WebView2Backend` renvoie des erreurs explicites ; test confirme indisponibilité | Implémenter et qualifier sur Windows avec SDK/runtime identifiés |
| BROWSER-02 — politique d'origine/redirects/permissions | `PARTIAL` | `url_origin.hpp` canonicalise/teste les origines HTTP(S) exactes et rejette suffixe, userinfo, jokers et IP non standard ; pas de backend pour intercepter redirects, popups, sous-ressources ou ports | Intégrer au vrai WebView2 et contrôler toute la navigation/dépendances avant d'ouvrir une session |
| MEDIA-01 — capture réelle→encodage→transport→texture | `NOT_IMPLEMENTED` | aucun module media ; aucun pixel de navigateur transmis | Créer le pipeline uniquement après Worker réel |
| MEDIA-02 — format wire média portable et borné | `PARTIAL` | codec v1 big-endian RGBA8 / input Q0.16 borné dans `engine/browser/remote_frame.hpp`; `engine/media/frame_queue.hpp` impose séquence, limites de frames/octets et drop-oldest pour frames complètes ; tests négatifs et harness ASan/UBSan | Relier à l'enveloppe/transport média, ajouter codec compressé maintenu et fuzzing Clang libFuzzer natif |
| INPUT-01 — interactions ciblées WebView2 | `NOT_IMPLEMENTED` | aucune injection réelle | Événements validés dans le WebView seulement ; jamais `SendInput` global |
| LAN-01 — test sur deux machines/modes A/B/C | `BLOCKED_BY_ENVIRONMENT` | aucune seconde machine disponible ; loopback uniquement | Exécuter parcours LAN physique, preuve réseau et révocation |
| PERF-01 — 5 invités, FPS/latence/débit/mémoire | `NOT_IMPLEMENTED` | aucune mesure réelle de streaming | Fixer machine de référence et mesurer avant d'annoncer capacité |
| DPI-01 — vrais moniteurs Windows multi-DPI | `BLOCKED_BY_ENVIRONMENT` | SDL dummy renvoie 1.00 ; le facteur est injecté | Test physique PerMonitorV2 100/125/150/200 % |
| PKG-01 — mécanisme publication Linux atomique / SHA / clean | `PARTIAL` | selftest GCC publie une fixture, vérifie hash, refuse l'écrasement et prouve que clean préserve releases ; ce n'est pas un package applicatif qualifié | Package réel Release, extraction/exécution sous Clang et contrôle de contenu |
| PKG-02 — package Windows/exécution hors MSYS2 PATH | `BLOCKED_BY_ENVIRONMENT` | aucun Windows natif ; pas d'archive Windows qualifiée | Build, extraire, inspecter DLL/licences, exécuter sur machine propre |
| DOC-01 — documentation utilisateur/sécurité/réseau cohérente | `PARTIAL` | README, BUILD, QUICKSTART, SECURITY, NETWORK, protocol, install/user guide et release notes corrigés | Relire docs restantes et synchroniser avec E2E final |
| REL-01 — release fonctionnelle dans `releases/` | `NOT_IMPLEMENTED` | aucun produit de partage qualifié dans `releases/` ; selftest retire sa fixture | Ne publier qu'après WebView2/pipeline réel + gates natifs, LAN et package |
| PR-01 — préparation à revue | `PARTIAL` | rapport/matrice/proofs disponibles, mais changements larges non committés et gates ouverts | Revue diff complète, CI officielle et décision utilisateur avant PR |

## E. Tests, commandes et outils

### GCC local — résultats actuels

Commande de test :

```sh
./.cache/build_local_synced.sh test debug --headless
```

Adaptateur temporaire neuf créé depuis le `build.sh` du worktree synchronisé ; il
change uniquement la résolution/validation du compilateur en g++/gcc et adapte le
probe de toolchain, sans modifier le script officiel. Compilateurs observés :
`/usr/bin/g++` et `/usr/bin/gcc` GCC 12.2. La copie et son probe restent dans `.cache/`.
Commandes exactes :

```sh
./.cache/build_local_synced.sh test debug --headless
./.cache/build_local_synced.sh selftest
```

Preuves :
- `docs/reports/evidence/continuation-synced-gcc-test.log` — test après fast-forward, correctif URL et code courant ;
- `docs/reports/evidence/continuation-synced-gcc-selftest.log` — selftest après réparation de la CLI fixture ;
- `docs/reports/evidence/continuation-synthetic-cli-regression.log` — première réexécution selftest après fast-forward échouée exit 2, cause identifiée/corrigée ;
- les logs `continuation-origin-policy-*` et `continuation-final-*` restent des runs précédents, non preuve du nouvel HEAD.

Les runs GCC exploratoires précédents sont conservés séparément. Après l'incrément
codec v1, GCC a passé unit 38/21 168, platform 20/760, IPv6 2/41, 15 étapes et
0 ignorée (`continuation-media-codec-gcc-test.log`). Un harness ASan/UBSan GCC a
passé 250 000 entrées aléatoires et les mutations de frames/input valides
(`continuation-media-codec-gcc-sanitizer.log`), sans être libFuzzer.

### Clang natif Linux via scripts officiels

Le runtime initialement installé via `llvm-runtime-clang-linux-x64@23.1.6-1`
contenait clang++ 23.1.5 mais omettait les resource headers. Un premier doctor a
échoué sur `stddef.h`; les tests Clang suivants ont révélé d'abord le warning
`std::stable_sort` déprécié sous libstdc++ `-Werror`, puis les headers SIMD GCC
incompatibles quand ils étaient injectés globalement. Ces échecs sont conservés
sous `continuation-official-clang-doctor.log`,
`continuation-official-clang-test.log`, `continuation-official-clang-test-retry.log`
et `continuation-official-clang-test-final.log`. Le code ne désactive aucun warning :
`HostServer::build_chat()` emploie désormais un insertion-sort stable borné (<=200
lignes), et les champs d'état fantômes de `WebView2Backend` ont été retirés.

Les headers builtin upstream Clang 23.1.5 ont ensuite été provisionnés depuis le
paquet npm `@cpp-linter/clang-tidy-linux-x64@23.1.5`, dans `.cache/` seulement ; le
wrapper temporaire ne fait qu'appeler clang/clang++ sans ajouter d'en-têtes GCC.
`./build.sh doctor --core-only` a alors réussi (exit 0), puis le doctor a été
rejoué avec les wrappers propres (sans include GCC) ; preuves historique et propre
conservées séparément. Les scripts officiels
`./build.sh test debug --headless` et `./build.sh test release --headless` ont réussi
(exit 0 chacun, 39/21 193 unit, 20/760 platform, IPv6 2/41, 15 étapes, 0 ignorée).
Le selftest officiel `./build.sh selftest` a aussi réussi (exit 0). Preuves finales :
`continuation-official-clang-doctor-retried.log` (historique, wrapper avec include GCC),
`continuation-official-clang-doctor-clean-resources.log` (retest propre, sans include GCC),
`continuation-clang-provisioning.txt` (versions, intégrité npm, licences et limites),
`continuation-official-clang-test-debug-final.log`,
`continuation-official-clang-test-release-final.log` et
`continuation-official-clang-selftest-final.log`. Tous s'exécutent sur un worktree sale
basé sur le commit 1a70c66, pas sur un commit de qualification ; la toolchain est
traçable par le reçu `continuation-clang-provisioning.txt`, mais les archives restent
locales et ne sont pas vendoriées. `--headless` ne
prouve ni fenêtre native, ni package, ni WebView2, ni Windows UCRT64.

Un premier appel isolé de `selftest` a retourné 1 parce que le binaire de tests
requis n'existait pas encore ; après le build/test explicite, la même étape `selftest`
a abouti à `SELFTEST PASS`. Cela a été un prérequis manquant, pas un échec de test
accepté. Les journaux finaux contiennent uniquement la séquence réussie.

Le `SKIP tls13` dans le sous-test à runtime forcé absent est le scénario négatif
attendu ; l'auto-test prouve le refus de session sans runtime. Le résultat n'est pas
une qualification Clang ou Windows.

### Ressources ICO/PE

`tests/build/toolchain_probe.cpp` se compile avec les options strictes C++20 dans
l'adaptateur local. Le probe a accepté l'ICO réel à 7 entrées et la fixture PE32+ x64
valide, puis a rejeté le faux ICO, le mauvais nombre d'entrées, le groupe d'icônes
malformé, le manifeste absent et la version divergent. Ces fixtures valident le
parseur/sonde, **pas** un EXE compilé par Windows UCRT64.

### DPI

La sonde et son rapport reproductibles sont :

- `tests/build/dpi_typography_probe.cpp` ;
- `tests/build/bmp_to_png.py` ;
- `docs/reports/evidence/dpi-multi-scale.txt` ;
- captures `docs/reports/evidence/dpi-captures/dpi-{100,125,150,200}.png`.

Exécuté avec SDL dummy/software ; `SDL_window_scale=1.00` dans tous les cas. C'est
une preuve de rendu logiciel aux tailles typographiques injectées, pas de DPI
physique.

### Doctor et limites de qualification

Le doctor officiel a d'abord échoué faute de clang++, puis un LLVM Clang 23.1.5 a été
provisionné dans `.cache/` et validé par `./build.sh doctor --core-only` (exit 0).
Le runtime est le paquet npm `llvm-runtime-clang-linux-x64@23.1.6-1`; ses resource
headers Clang 23.1.5 viennent de `@cpp-linter/clang-tidy-linux-x64@23.1.5` selon
l'intégrité npm. Le tout est local, temporaire et non ajouté au dépôt. Les scripts
officiels Debug/Release/tests/selftest passent avec ces chemins. Cela valide la
compilation native Linux Clang, mais ne remplace pas une installation système
reproductible ou la CI du SHA committe. `build.sh deps --fetch` n'a pas été relancé ;
les archives SDL/ImGui existantes ont été rehashées par les tests. `--headless` est
explicite : aucune fenêtre X11 native, build Windows, package produit ou WebView2
n'est annoncé PASS.

## F. Qualification physique et mesures

| Critère | Résultat |
|---|---|
| Deux ordinateurs sur LAN | Non exécuté — seconde machine absente |
| WebView2 réel et page dynamique | Non exécuté — backend absent |
| Modes A/B/C en E2E | Non exécuté |
| 5 invités, 720p/FPS, latence p95, mémoire | Aucune mesure |
| Coupure/réseau/révocation sur machines distinctes | Tests loopback seulement ; physique non vérifié |
| Windows Clang++ UCRT64/PE/installation | Bloqué — plateforme absente |
| Moniteurs 100/125/150/200 % physiques | Non exécuté |

## G. Build, releases et gouvernance de livraison

Les scripts contiennent des chemins de package/publication atomique et un selftest de
préservation de `releases/`. En revanche, aucun package réel n'a été reconstruit,
extrait et exécuté avec la chaîne officielle durant cette continuation. `releases/`
ne contient aucun produit qualifié. Les essais de fixture ne sont pas des releases.

Cinq commits existaient déjà sur la branche distante et ont été conservés via
fast-forward. Dans cette continuation, aucun nouveau commit ni push n'a été créé ;
aucune PR n'existe pour cette branche (vérifié avec `gh pr list`). Aucun merge n'a eu
lieu. La branche n'est **pas prête pour un merge** ; un push/PR de cette itération
reste à faire après revue des changements staged et qualification CI/native.

## H. Critères de réussite F01–F20 du mandat courant

Évaluation du **worktree courant non commité**, et non preuve attachée au HEAD
seul. Le SHA de test ne peut donc pas encore être enregistré comme un commit
release. Aucun critère de produit n'est classé `VERIFIED_ON_CURRENT_HEAD` à ce stade.
Les tests GCC ci-dessus qualifient seulement les composants exploratoires du
worktree ; aucun ne se substitue à une porte Clang/UCRT64 ou physique.

| ID | Statut | Éléments et preuve manquante |
|---|---|---|
| F01 Toolchain officielle | `PARTIAL` | `build.sh doctor --core-only` passe avec upstream LLVM Clang 23.1.5 et resource headers 23.1.5; paquet npm/runtime wrappers temporaires détaillés §E. Pas de chaîne Windows UCRT64/CI sur le worktree. |
| F02 Build complet Debug/Release | `PARTIAL` | scripts officiels Clang Debug et Release `--headless` ont compilé et testé le worktree ; aucun native-ui X11, package/extraction, Windows ni SHA commité. |
| F03 Tests projet | `PARTIAL` | Clang Debug/Release : unit 39/21 193, platform 20/760, IPv6 2/41 ; 15 étapes, 0 skip. GCC adapter également exécuté ; harness ASan/UBSan GCC 250k inputs. Pas de Clang sanitizer/libFuzzer/Windows. |
| F04 WebView2 réel | `NOT_IMPLEMENTED` | backend renvoie explicitement `PlatformError`; pas de contrôleur/navigation DOM. |
| F05 Capture réelle | `NOT_IMPLEMENTED` | aucune surface WebView2 ni pixel du contenu source. |
| F06 Encode/décode réel | `NOT_IMPLEMENTED` | aucun pipeline média connecté. |
| F07 Affichage de vraie frame invitée | `NOT_IMPLEMENTED` | aucune frame réseau d'une page réelle. |
| F08 Entrées réelles | `NOT_IMPLEMENTED` | aucun input router WebView2; Demo backend ne fait qu'accroître un compteur. |
| F09 Mode A, profils/stockage réels isolés | `NOT_IMPLEMENTED` | modèle et dossiers de démo ne sont pas des profils navigateur. |
| F10 Mode B, bail exclusif opérationnel | `PARTIAL` | contrôle de rôle/bail dans le modèle testé en unit ; aucun flux input réel/canal média. |
| F11 Mode C, rejet serveur d'input forgé | `PARTIAL` | capacité de rôle restrictive dans le modèle ; le serveur de production ne reçoit pas de média/inputs réels. |
| F12 TLS 1.3 + identité de pair | `PARTIAL` | OpenSSL TLS 1.3 PSK loopback, mauvais secret/downgrade/refus-runtime testés dans les suites locales ; pas Windows, LAN, revue crypto ni qualification du SHA. |
| F13 Ports/origines/ressources sûrs | `PARTIAL` | correctif de canonicalisation d'origine exact unit-testé ; pas de WebView navigation/subresource enforcement. Proxy/réseau pas qualifié physiquement. |
| F14 Rollback/stop/revocation/cleanup | `PARTIAL` | moteur/fixtures ont des tests, mais pas de processus/profile WebView réel ni E2E. |
| F15 LAN à deux machines | `BLOCKED_BY_ENVIRONMENT` | seconde machine absente ; aucun test physique exécuté. Voir `REAL_LAN_QUALIFICATION.md`. |
| F16 Windows/UCRT64 build/test/package | `BLOCKED_BY_ENVIRONMENT` | runner Windows absent de cette exécution ; anciens logs autres commits seulement. Voir `WINDOWS_UCRT64_QUALIFICATION.md`. |
| F17 DPI physique multi-moniteur | `BLOCKED_BY_ENVIRONMENT` | aucune machine/écran Windows ; facteur injecté SDL dummy n'est pas preuve physique. |
| F18 Archive extraite et exécutée | `BLOCKED_BY_ENVIRONMENT` | pas de package applicatif officiel construit ici ; fixture d'auto-test ne compte pas. |
| F19 Branding exécutables PE + fenêtre/taskbar | `PARTIAL` | exports ICO vérifiés et probe PE synthetic ; aucun PE candidate current build inspecté nativement. |
| F20 Documentation | `PARTIAL` | README/build/security/quickstart/NETWORK et audit révisés ; runbooks ajoutés ; contradictions historiques restent signalées et la qualification doit être liée à un commit. |

**Conclusion des portes : `NOT_FINALIZED`.** La composante Linux Clang Debug/Release
et les tests headless passent sur le worktree, mais pas sur un commit qualifié. Les
gates F04–F09, F15–F18 restent ouverts : WebView2/pipeline réel, LAN, Windows/UCRT64,
DPI physique et archive extraite. L'outil Clang local provient de paquets npm et
n'est pas encore une provision CI reproductible attachée au SHA. La présence d'une
workflow Windows/Linux dans `.github/workflows/build.yml` est une capacité
d'automatisation, pas une exécution réussie du code local non poussé.

## I. Risques de sécurité résiduels

1. Worker navigateur absent ; pas de frontière d'origine exécutable.
2. Aucun transport média ni preuve de chiffrement end-to-end des pixels.
3. Transport Windows indisponible ; le contrôle TLS Linux n'est pas un audit
   indépendant.
4. Proxy HTTP non qualifié pour réseau non fiable ; limiter/examiner les destinations
   et la révocation.
5. Le codec `remote_frame.hpp` est désormais portable et borné pour les frames brutes et les inputs, mais il n'est pas relié au transport, au navigateur ou au rendu ; aucune vidéo réelle n'est transmise.
6. Pas d'E2E physique des permissions; rôle/état UI ne prouve pas un contrôle
   anti-client malveillant sur une vraie connexion.
7. PSK sans confidentialité persistante annoncée ; limites décrites dans ADR-016.

## J. Actions bloquantes, ordre conseillé

1. Obtenir Clang++ officiel >=18 pour Linux et une machine Windows native MSYS2
   UCRT64 ; refaire tous les builds/scripts après les corrections des probes.
2. Définir/installer officiellement le SDK et runtime WebView2 ; implémenter COM/STA,
   profils indépendants, allowlist, création/destruction asynchrones et navigation.
3. Prouver une vraie capture du contenu web et sécuriser le proxy/destinataire.
4. Spécifier le format média portable, l'encodage, backpressure et transport ; créer
   le rendu invité.
5. Implémenter injection ciblée et vérifier modes A/B/C côté serveur avec clients
   forgés.
6. Qualifier TLS Windows et auditer authentification/dérivation/capacités.
7. Faire l'E2E physique LAN à deux machines, puis charge multi-invités.
8. Qualifier DPI Windows physique et ressources de chaque PE distribué.
9. Construire/extrair/inspecter/exécuter une Release candidate, vérifier hash et
   licenses, puis seulement envisager une PR.
