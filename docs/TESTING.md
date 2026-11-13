# Validation et reproduction

Le [rapport daté](reports/VALIDATION.md) est la source des **résultats exécutés**.
Ce document explique le plan, les commandes et ce que les tests ne prouvent pas.

## 1. Suites automatisées

Les checks du harness sont actifs en Debug **et** Release : pas d'`assert` qui
supprime les vérifications avec `NDEBUG`. `REQUIRE` interrompt le cas en échec,
`CHECK` le compte ; le binaire retourne 1 s'il reste un échec.

| Entrée CTest | Contenu |
|---|---|
| `unit` | 26 cas : erreurs, types, arbres/cycles/PPID, classification, registre, snapshots partiels, capacité, sessions, rôles, DM, quotas, UTF-8, codec, estimation et audit |
| `platform` | Entropie OS, réservation IPv4 TCP/UDP, concurrence d'un second bind, tables réelles, processus, interfaces ; fixtures proc et comptage FD sous Linux |
| `ipv6` | Réservation TCP/UDP v6, V6ONLY, tables et PID ; code **77 = SKIP** si IPv6 n'est pas disponible |
| `cli-version` | Binaire installé candidat exécutable et version non trompeuse |
| `cli-arguments` | Cinq contrats : help/version = 0, options inconnues/supplémentaires/ambiguës = 2 |
| `desktop-smoke` (UI) | SDL `dummy` + software, construction ImGui, observation publiée, au moins huit frames et fermeture |
| `desktop-errors` (UI) | Arguments refusés, capture hors smoke refusée, driver absent, ouverture BMP impossible ; disque plein via `/dev/full` sous Linux |

Résultat Linux actuel par suite : `unit` **26 / 12 449**, `platform` **6 / 284**,
`ipv6` **2 / 41** (cas / checks), soit **34 cas / 12 774 checks**, plus les contrats
CLI et UI. Ce sont des checks d'un même ensemble de tests, pas 12 774 scénarios
utilisateurs indépendants. Les cas propres à Linux ne s'exécutent pas sous Windows.

### Régressions de sécurité et robustesse couvertes

- Parents absents, cycles, parent créé après enfant ; noms exacts et alternatives.
- Port sélectionné disparu mais autre port encore actif ; PID réutilisé ; identité inconnue.
- **Complétude des deux observations de processus**, pas seulement de la table réseau.
- Échec de capacité ou du provider : sélection invalidée, jamais rétablie automatiquement.
- Queue du worker limitée à 32 ; anciens snapshots immuables ; historique borné à 4096 groupes.
- Expiration à la milliseconde exacte ; fermeture coupe les capacités ; nettoyage incomplet refuse Closed.
- Contexte indépendant non partagé, contrôle collaboratif exclusif, révocation/reconnexion.
- Réponse à un DM toujours privée, destinataire autorisé, reader explicite, rate/volume quotas.
- Chaînes UTF-8 non canoniques/contrôles ; routes de feedback omises ; screenshot non supporté refusé.
- Layout big-endian, limites avant allocation, toutes les troncatures, types/flags/versions inconnus.
- 10 000 mutations déterministes de l'enveloppe en plus de la cible libFuzzer.
- Réservation port zéro conservée, fermeture/déplacement RAII et seconde réservation concurrente refusée.
- Ancienne fuite du chemin `SDL_SaveBMP` en erreur : le test capture et refuse les diagnostics ASan/LSan,
  même si le code de sortie attendu est déjà 1. Aucun suppressions file ou `detect_leaks=0`.

## 2. Reproduction native

Windows d'abord, avec le SDK/STL Microsoft :

```bat
build.bat clang-debug
build.bat clang-release
build.bat clang-ui
```

Sur Linux, pour le cœur :

```bash
./build.sh clang-debug
./build.sh clang-release
./build.sh clang-asan
ctest --preset clang-asan -V
ctest --test-dir out/build/clang-asan -R '^unit$' --repeat until-fail:25 --output-on-failure
python3 tools/check_headers.py
```

Les 25 répétitions ont pour objet d'augmenter les chances de détecter une erreur
de concurrence ; elles ne remplacent pas un test de charge ni ThreadSanitizer.
Le checker de headers compile chaque header seul, avec le compilateur natif
`CXX` (par défaut clang++). Il n'utilise pas un PCH ou une inclusion accidentelle.

### UI et sanitizers sans écran

```bash
cmake -S . -B out/build/ui-asan -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DPLPT_BUILD_UI=ON \
  -DPLPT_HEADLESS_UI=ON -DPLPT_SANITIZE=ON
cmake --build out/build/ui-asan --parallel 4
ctest --test-dir out/build/ui-asan -V
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
  out/build/ui-asan/plpt-desktop --smoke-test --screenshot out/desktop.bmp
```

ASan/UBSan instrumentent notre code lié à `plpt_options`. SDL/ImGui sont compilés
séparément sans cette instrumentation complète ; LeakSanitizer intercepte néanmoins
leurs allocations, ce qui a permis de détecter la fuite du chemin d'erreur BMP.
Il ne faut donc pas annoncer un audit exhaustif des bibliothèques tierces.

Les échecs de renderer, ouverture/écriture/fermeture de capture retournent 1 ;
les erreurs d'arguments retournent 2. L'UI normale ne crée aucune capture.

## 3. Fuzzing du codec

```bash
cmake --preset clang-fuzz
cmake --build --preset clang-fuzz --target plpt-fuzz-envelope --parallel 4
# Le script refuse d'écraser un corpus existant. Choisir un nouveau dossier par run.
python3 tools/seed_fuzz_corpus.py out/fuzz/fresh-corpus
out/build/clang-fuzz/plpt-fuzz-envelope \
  -runs=20000 -seed=20261113 -max_len=4096 -timeout=2 \
  -artifact_prefix=out/fuzz/ out/fuzz/fresh-corpus
```

Le target décode des octets, vérifie l'encodage canonique des messages acceptés et
refuse un roundtrip incohérent. Deux seeds (`Hello`, `Chat`) atteignent les branches
valides, pas seulement le rejet du magic. Résultat de ce run : **smoke court**, pas
campagne de fuzzing longue ni preuve de sécurité du protocole. La limite 4096 de ce
run n'explore pas tout le plafond d'une future enveloppe vidéo de 262144 octets.
Conserver hors Git les corpus volumineux et crash artifacts ; minimiser puis ajouter
un cas de régression au dépôt en cas de découverte.

## 4. Vérification croisée Windows — complément seulement

```bash
python3 tools/check_windows_syntax.py \
  --clang /chemin/vers/clang++ \
  --zig-lib /chemin/vers/ziglang/lib \
  --sdl-root /chemin/vers/SDL-commit-epingle \
  --imgui-root /chemin/vers/imgui-commit-epingle
```

Ce script utilise Zig 0.14.1 comme **jeu de headers** et clang++ comme compilateur,
avec target MinGW, `-fsyntax-only` et `-fexperimental-library` (nécessaire au
`std::jthread` de ce libc++). Il ne lie aucun `.exe`, ne vérifie pas la STL/ABI
Microsoft, les manifestes, CMake Windows ni les DLL. Il ne ferme **aucune** porte
d'exécution Windows du maître.

## 5. Validation manuelle Windows — NON EXÉCUTÉE ici

1. **Découverte réelle** : Next.js/Vite, PowerShell, CMD, IDE, Docker Desktop/WSL2,
   processus à accès refusé, noms Unicode, arrêts rapides et changement de port.
2. **UI** : navigation des trois pages, filtre positif/négatif, sélection TCP vs
   UDP, protection, disparition du service pendant une action, fermeture immédiate.
3. **Affichage/entrées** : Windows 10/11, DPI 100/150/200 %, deux écrans, clavier,
   focus, minimisation, GPU/renderer matériel. Vérifier l'absence de console parasite.
4. **Installation** : VM propre sans LLVM/SDK, utilisateur standard, manifeste
   asInvoker, DLL, signature, fichiers verrouillés, désinstallation sans perte de données.
5. **Sécurité future** : paquets forgés, invitation rejouée, mauvaise identité,
   entrée d'un observer, mauvaise session, SSRF et refus de fichiers/downloads.
6. **Scénario cinq invités** : vrai Wi-Fi, profils/cookies isolés, backend commun,
   latences mesurées, révocation et nettoyage normal/après crash.

Les items 5–6 exigent les lots E–I qui ne sont pas intégrés ; ils ne peuvent pas
être remplacés par les cinq participants du modèle unitaire. Pour chaque case,
consigner machine/version/commande/résultat/artefact, y compris SKIP et échec.
