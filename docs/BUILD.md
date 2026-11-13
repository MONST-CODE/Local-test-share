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
set "PLPT_MSYS2_ROOT=D:\outils\msys64"
```

Le compilateur devient exactement `%PLPT_MSYS2_ROOT%\ucrt64\bin\clang++.exe`.
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
set "PLPT_IMGUI_DIR=C:\dev\imgui"
build.bat deps --fetch
build.bat doctor --verbose
build.bat debug
```

`PLPT_IMGUI_DIR` est la **racine contenant `imgui.h`, les `.cpp` et `backends/`**,
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
build.bat deps --fetch
build.bat doctor --verbose
build.bat debug
build.bat release
build.bat test
build.bat test debug --native-ui
build.bat test release --native-ui
build.bat test asan --native-ui
build.bat fuzz
build.bat ide-check
build.bat selftest
build.bat clean
build.bat package
```

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

`package` reconstruit Release, teste, archive, extrait, compare et exécute. Windows
inspecte récursivement les imports PE, copie uniquement les DLL UCRT64 nécessaires
avec provenance/hash/notices, puis exécute avec **PATH système uniquement**.
Les DLL OS/UCRT Windows ne sont pas copiées. Le package Windows est nommé
`PLPT-0.1.0-preview-windows-ucrt64-x64.zip`. Linux reste lié à son ABI glibc/libstdc++/X11.

Archives de développement, pas installateurs signés. Voir `docs/QUICKSTART.md`,
les trois rapports `docs/reports/UCRT64_*.md` et les preuves Linux associées.
Les anciennes preuves MSVC/SDK sont historiques, pas une qualification UCRT64.
