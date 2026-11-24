# Build direct — Windows MSYS2 UCRT64, Linux clang++

**Seuls `build.bat` et `build.sh` définissent et exécutent le build.** Aucun Jenga,
CMake, Ninja, Make, MSBuild, CTest, CPack, moteur intermédiaire ou orchestrateur
Python/PowerShell. Les tâches VS Code délèguent ; elles ne définissent aucun target.

## Windows — environnement exact du propriétaire

Compilateur par défaut, **sans recherche dans PATH** :

```text
C:\msys64\ucrt64\bin\clang++.exe
```

Un autre emplacement MSYS2 est permis uniquement par :

```bat
set "DEVLINK_MSYS2_ROOT=D:\outils\msys64"
```

Le compilateur devient exactement `%DEVLINK_MSYS2_ROOT%\ucrt64\bin\clang++.exe`
(`PLPT_MSYS2_ROOT` reste accepté comme ancien nom).
Aucun `PLPT_CXX`, standalone LLVM, CLANG64, MINGW64/MSVCRT, compilateur MSYS `/usr`,
clang-cl, MSVC ou découverte automatique de secours. Ne pas utiliser un Developer
Command Prompt Visual Studio. Un terminal **cmd ordinaire**, y compris dans VS Code,
suffit. Depuis PowerShell dans VS Code, `cmd /d /c build.bat debug` délègue au même
script ; aucun script PowerShell n'orchestre le build.

Prérequis MSYS2 **UCRT64** (à installer seulement s'ils manquent, via pacman dans
le terminal MSYS2 UCRT64 ; suivre la procédure de mise à jour MSYS2 avant installation) :

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-binutils mingw-w64-ucrt-x86_64-libarchive mingw-w64-ucrt-x86_64-sdl3
# Tests instrumentés, si disponibles pour ce même environnement :
pacman -S --needed mingw-w64-ucrt-x86_64-compiler-rt
```

Les dépendances de ces paquets fournissent MinGW-w64, UCRT et GNU libstdc++.
La présence de bibliothèques/outils GCC dans l'installation ne signifie pas que
PLPT utilise gcc/g++ : **toutes les compilations C++ et éditions de liens passent
par le seul clang++.exe sélectionné**. Les versions réellement installées sont
rapportées par `doctor`; la version SDL attendue est strictement **3.4.20-1**.
Si un miroir propose une autre version, ne pas la considérer comme le pin qualifié :
le script échoue précisément. Voir le ledger pour l'archive exacte et son hash.

Le script ne télécharge ni n'installe un SDK Microsoft. Il ne lance ni vswhere,
VsDevCmd, rc, mt, dumpbin ou lld-link. Il place les outils/DLL UCRT64 devant les
utilitaires système dans son PATH local et neutralise les variables SDK héritées.
Les variables d'injection CPATH/CPLUS_INCLUDE_PATH/LIBRARY_PATH, etc. sont refusées.

### Dépendances déjà présentes sur votre PC

SDL3 est utilisée **depuis l'installation UCRT64** : `include/SDL3`,
`lib/libSDL3.dll.a`, `bin/SDL3.dll`. Le script vérifie l'archive pacman épinglée,
puis compare les headers, import library, archive statique et DLL installés à ses
bytes. Il ne remplace pas votre SDL et n'utilise jamais `SDL3.lib`.

Pour votre copie existante d'ImGui :

```bat
set "DEVLINK_IMGUI_DIR=C:\dev\imgui"
build.bat deps --fetch
build.bat doctor --verbose
build.bat debug
```

`DEVLINK_IMGUI_DIR` (ancien nom accepté : `PLPT_IMGUI_DIR`, celui que lit la
configuration IntelliSense de VS Code) est la **racine contenant `imgui.h`, les `.cpp` et `backends/`**,
pas le sous-dossier backends. Les fichiers utilisés doivent correspondre au pin
ImGui 1.93.0 ; ils sont comparés à l'archive vérifiée et ne sont pas modifiés.
Un ImGui différent ou un imconfig personnalisé demande une révision explicite du
pin, pas une acceptation silencieuse. Sans cette variable, chaque build extrait
sa propre copie vérifiée sous `out/windows/<configuration>/vendor/imgui/`.

```cpp
#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
```

`deps` réutilise l'archive SDL du cache pacman si présente. `deps --fetch` autorise
explicitement le téléchargement des deux archives exactes ; il **n'installe ni
ne met à jour** les paquets du propriétaire. Debug/release/test/package/doctor ne
téléchargent rien. Un fichier absent/corrompu donne une erreur, pas une autre version.

### Commandes officielles Windows

```bat
./build.bat deps --fetch
./build.bat doctor --verbose
./build.bat debug
./build.bat release
./build.bat test
./build.bat test debug --native-ui
./build.bat test release --native-ui
./build.bat test asan --native-ui
./build.bat fuzz
./build.bat ide-check
./build.bat selftest
./build.bat clean
./build.bat package
```

### Lire la sortie du build

Chaque opération affiche le bandeau DevLink (dégradé marron de la marque), puis une
ligne par **phase réelle**, numérotée `[n/N]` :

```text
  √  [1/11] Contrôle de la chaîne de compilation UCRT64    clang 22.1.8 · x86_64-w64-windows-gnu · 11,9 s
  √  [4/11] Compilation du moteur DevLink                  30 fichiers · 55,7 s
  •  [5/11] Compilation des applications et de l'interface 7/12 · imgui_widgets.cpp
```

| Opération | Étapes |
|---|---|
| `debug`, `release` | 8 : chaîne, dépendances, préparation (ImGui, manifeste, icône, version), moteur, applications, liens, contrôle PE, DLL et licences |
| `test`, `asan` | 11 : les 8 précédentes + compilation des suites, indépendance des en-têtes, exécution des tests |
| `package` | 14 : `test release --native-ui` + assemblage, publication atomique, vérification de l'archive publiée |
| `doctor` / `deps` | 3 / 3 |
| `selftest`, `fuzz`, `ide-check`, `clean` | 7, 5, 4, 1 |

- `√` réussite, `○` étape non exécutable ici (jamais comptée comme réussie : SKIP,
  code 77), `×` échec. L'étape en cours affiche sa progression (`7/12 · fichier`)
  sur une seule ligne réécrite.
- Après l'exécution des tests, une ligne par suite résume cas et vérifications.
- Le résumé final donne le verdict, la durée, le dossier des binaires (ou
  l'archive) et le journal.
- **Aucune sortie d'outil n'est affichée en temps normal** (pacman, certutil,
  objdump, fc…). Chaque commande est capturée en entier sous `out\windows\logs\`
  (`<opération>-<mode>-….log`, plus un fichier `.command-N.txt` par commande).
- **En cas d'échec**, l'étape passe en `×`, puis la sortie de la commande fautive
  est affichée **telle quelle, sans indentation** : le problem matcher `$gcc` de
  VS Code relie toujours chaque erreur à sa ligne de code. Le chemin de la capture
  complète suit. Pour un test : ses assertions, preuves et bilan.
- `--verbose` rétablit l'affichage intégral de chaque commande et de sa sortie.
- `--plain`, la variable `NO_COLOR` (norme no-color.org) ou `DEVLINK_PLAIN=1`
  retirent couleurs et réécriture de ligne. En CI (`GITHUB_ACTIONS=true`), la
  progression est linéaire ; les annotations `::notice`/`::error` sont inchangées.
- La console passe en UTF-8 (page de codes 65001) pendant l'opération et retrouve
  sa page d'origine à la sortie, même en cas d'échec. Les glyphes (`√ × ○ • ·` et
  les blocs du bandeau) existent dans Consolas et Lucida Console : ils s'affichent
  aussi dans la console Windows classique, sans police particulière.
- Un build interrompu (Ctrl+C) laisse le verrou `out\windows\.build-lock` ; le
  build suivant l'indique et donne la commande pour le retirer, après avoir vérifié
  qu'aucun build ne tourne.

`build.sh` applique le même modèle sous Linux (bandeau partagé :
`tools/build/devlink-banner.txt`). Hors terminal (tube, fichier, CI), sa sortie
reste linéaire ; une commande dont l'appelant redirige la sortie la reçoit
toujours en entier.

`doctor` vérifie le chemin, `--version`, le triple **x86_64-w64-windows-gnu**,
`-print-search-dirs`, les chemins réels des headers/librairies, GNU ld, UCRT et
libstdc++, puis compile/lie/exécute un probe C++20/threads/BCrypt natif. Les chemins
MSVC/SDK/MINGW64/CLANG64/MSYS sont refusés. Les configs implicites Clang sont désactivées.

Debug : `-O0 -g`; Release : `-O2 -DNDEBUG`; C++20 et warnings stricts sur PLPT.
Le driver clang++ appelle GNU ld de ce même UCRT64 avec les imports Win32 `.a`.
Windres utilise **clang++ comme préprocesseur** pour embarquer le manifeste existant.
Un probe Win32 lit ensuite la ressource #1 de chaque EXE PLPT et vérifie asInvoker
et PerMonitorV2. Ces contrôles ne remplacent pas un essai manuel DPI/compte standard.

ASan/libFuzzer nécessitent les runtimes de ce même UCRT64. `doctor` indique leur
présence ; une absence retourne **SKIP/77**, jamais PASS. La CI n'exécute ces profils
que si les fichiers requis existent et échoue normalement si leur compilation ou
leurs tests échouent. SDL Windows est précompilée, non instrumentée par notre ASan.

## Visual Studio Code

Installer l'extension C/C++ pour IntelliSense. Ouvrir le dossier du dépôt :

- `Ctrl+Shift+B` : tâche Debug ; les autres tâches sont Release, Test et Clean.
- Toutes lancent `cmd.exe /d /c build.bat <opération>`, dans le dossier du dépôt.
- IntelliSense par défaut : **C:/msys64/ucrt64/bin/clang++.exe**, mode
  **windows-clang-x64**, C++20, découverte GNU/UCRT via le compilateur.
- Pour une racine ou un ImGui existant personnalisé, définir `PLPT_MSYS2_ROOT` et
  `PLPT_IMGUI_DIR` **avant de lancer VS Code**, puis sélectionner la configuration
  « Windows UCRT64 (custom root / existing ImGui) ». Pour la racine par défaut,
  définir `PLPT_MSYS2_ROOT=C:\msys64` dans cette configuration personnalisée.
- Les includes ImGui utilisent sa racine, avec `backends/` dans les directives.
  L'extraction par défaut devient disponible après `debug`.

`ide-check` contrôle nativement les commandes et paramètres des fichiers VS Code.
La CI exécute aussi les quatre commandes de tâches. Ce n'est pas une affirmation
de clics manuels dans VS Code ni de validation interactive de son extension.

## Linux — architecture conservée

Prérequis : clang/clang++, libstdc++ de développement, libx11-dev, libxext-dev,
curl/tar et utilitaires natifs. Exemple Ubuntu :

```sh
sudo apt-get install clang g++ libx11-dev libxext-dev curl tar openssl xvfb xauth
./build.sh deps --fetch
./build.sh doctor --verbose
./build.sh debug
xvfb-run -a ./build.sh test debug --native-ui
./build.sh release
xvfb-run -a ./build.sh test release --native-ui
xvfb-run -a ./build.sh test asan --native-ui
./build.sh fuzz
./build.sh selftest
./build.sh clean
xvfb-run -a ./build.sh package --native-ui
```

`g++` fournit les headers/librairies libstdc++, mais ne compile pas PLPT. Linux garde
son profil SDL source direct X11/software + dummy et ImGui SDL3/SDLRenderer3 ; aucun
paquet Windows n'entre dans ce build. `--headless` est un profil dummy explicite,
non distribuable, jamais une preuve d'affichage natif. `PLPT_CXX`/`PLPT_CC` restent
possibles **uniquement pour Linux**. ASan+UBSan y instrumentent aussi SDL/ImGui.

## Fiabilité, sorties et packages

`out/{windows,linux}/{debug,release,tests,packages,dependencies,doctor,logs}/`.
Chaque compilation supprime ses anciens objets/EXE/marker, vérifie les entrées et
recompile tout ; aucune incrémentalité fragile. Les `.d` sont conservés, pas utilisés
comme cache. Durées mesurées incluant validation/extraction/build, hors tests.
`--verbose` expose chaque commande et les diagnostics du driver. Les logs conservent
les invocations même sans ce flag. Un SKIP IPv6/77 ne devient pas un PASS.

Un lock bloque les builds concurrents. Nettoyage limité aux sorties générées,
refus des symlinks/reparse points. Les archives vérifiées et les logs survivent à
`clean`. Ne pas placer des sources/dépendances personnelles sous `out/` et ne pas
modifier un script pendant son exécution.

`package` est conçu pour reconstruire Release, exécuter les tests et smoke tests
natifs, créer l'archive, l'extraire dans un dossier vierge, comparer les fichiers
et exécuter les binaires extraits. Le script Linux publie le résultat validé dans
`releases/` avec sidecar SHA-256 ; le script Windows contient un chemin analogue,
mais il n'a pas été exécuté dans l'environnement de continuation courant.

**Statut de qualification (2026-11-21) :** sur un poste Windows 11 x64 avec la
chaîne MSYS2 UCRT64 officielle (clang 22.1.8), `doctor`, `test debug`,
`test debug --native-ui`, `test release --native-ui`, `selftest` et `ide-check` ont
été exécutés (`fuzz` : SKIP, libFuzzer absent ; `package` non exécuté) ; résultats dans
[`reports/IMPLEMENTATION_STATUS.md`](reports/IMPLEMENTATION_STATUS.md). `build.sh`
n'a pas été exécuté sur un Linux natif depuis la refonte de son affichage : sa
syntaxe et sa mécanique d'affichage sont vérifiées, pas une build Linux complète.
Le test de ressources PE via fixtures synthétiques valide le parseur uniquement.

Toute archive générée avant une qualification complète est une build de diagnostic,
pas une release de partage fonctionnelle. Linux reste lié à son ABI glibc/libstdc++/
X11. Les preuves `docs/reports/UCRT64_*.md` sont historiques et ne remplacent pas
une exécution sur la branche courante.
