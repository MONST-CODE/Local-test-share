# Compiler, tester et empaqueter PLPT

**Priorité : Windows x64. Compilateur : clang++ (pas clang-cl, pas g++).**
Ce guide distingue les commandes prévues pour Windows des commandes effectivement
exécutées dans le [rapport](reports/VALIDATION.md). Le sandbox de développement
actuel est Linux : il ne permet pas de prétendre avoir exécuté un `.exe` Windows.

## 1. Windows x64 — poste de développement

### Prérequis

1. Windows 10/11 x64. Cible de compilation `_WIN32_WINNT=0x0A00`.
2. LLVM officiel, **clang++ 22.1.8 visé** pour cette preview ; enregistrer la version
   réellement utilisée. 22.1.8 a exécuté les vérifications Linux/croisées, pas une
   qualification Windows. La CI initiale emploie le LLVM préinstallé du runner :
   elle vérifiera la compatibilité, pas une toolchain de release déjà figée.
3. Visual Studio 2022 Build Tools : charge de travail « développement Desktop C++ »,
   toolset MSVC x64 et Windows SDK 10 (19041 ou plus récent). **LLVM seul ne fournit
   ni la STL Microsoft ni les import libraries du SDK.** Il reste le compilateur ;
   les Build Tools fournissent les headers, bibliothèques et outils de lien/ressources.
4. CMake ≥ 3.24 et Ninja dans `PATH`.
5. Pour créer un installateur : NSIS **3.11**. Rien de cela n'est une dépendance
   d'exécution à installer par un invité.

Ouvrir un terminal développeur x64, non élevé. Vérifier :

```bat
where clang++
clang++ --version
cmake --version
ninja --version
where link
where rc
```

Le target attendu de clang++ sous Windows est `x86_64-pc-windows-msvc`. Ne pas mélanger
une STL MinGW avec les import libraries MSVC. Si CMake choisit l'ABI MinGW ou le
driver `clang-cl`, il refuse cette configuration plutôt que masquer la différence.

### Chemin recommandé avec script

Depuis la racine du dépôt :

```bat
build.bat clang-debug
build.bat clang-release
build.bat clang-ui
```

Le script utilise `vswhere`/`VsDevCmd` pour charger le SDK x64 si nécessaire. Chaque
échec de configuration, compilation ou test arrête le script. `clang-ui` est une
configuration **Release** avec SDL3 et ImGui. Elle télécharge au build des archives
épinglées par commit **et hash** ; aucune récupération n'a lieu au lancement du produit.

### Commandes explicites, sans wrapper

Dans un **x64 Native Tools Command Prompt** :

```bat
cmake --preset clang-debug
cmake --build --preset clang-debug --parallel
ctest --preset clang-debug --output-on-failure
out\build\clang-debug\plpt-monitor.exe

cmake --preset clang-ui
cmake --build --preset clang-ui --parallel
ctest --preset clang-ui --output-on-failure
out\build\clang-ui\plpt-desktop.exe
```

Équivalent sans preset :

```bat
cmake -S . -B out\build\windows-manual -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release -DPLPT_BUILD_UI=ON -DBUILD_TESTING=ON
cmake --build out\build\windows-manual --parallel
ctest --test-dir out\build\windows-manual --output-on-failure
```

Liaisons Windows du cœur : `ws2_32`, `iphlpapi`, `bcrypt`. `SOCKET` n'est pas tronqué.
Le manifeste demande `asInvoker`. Le runtime C/C++ Microsoft est sélectionné en
statique (`MultiThreaded`, variante Debug pour Debug), comme SDL3 et ImGui, pour
réduire les DLL à déployer. Vérifier tout de même les dépendances réelles du binaire
Release avec `dumpbin /DEPENDENTS` : ne jamais supposer que le packaging est correct.

Le smoke test desktop de CTest force le driver SDL `dummy`/renderer `software`.
Il prouve l'initialisation, des frames et l'arrêt, **pas** le rendu D3D, les interactions,
les claviers IME ou les écrans Windows. Ces tests natifs restent dans la matrice.

## 2. Packaging Windows

### Archive portable de développement

Après `build.bat clang-ui` réussi :

```bat
cmake --install out\build\clang-ui --prefix out\stage-windows
cpack --config out\build\clang-ui\CPackConfig.cmake -B out\packages
```

L'archive ZIP contient le moniteur CLI, l'UI, la documentation et les notices tierces.
Ne pas empaqueter un build Debug pour des testeurs. `out/` est ignoré par Git et ne
doit pas devenir un stockage de grosses releases dans le dépôt.

### Installateur NSIS, par utilisateur

Dans PowerShell, après compilation Release avec UI :

```powershell
# Depuis la racine du dépôt. Aucun contournement de la stratégie d'exécution.
.\tools\package-windows.ps1
```

Le script compile à nouveau si nécessaire, exécute CTest, prépare le staging,
vérifie NSIS 3.11, génère l'installateur et calcule son SHA-256. Équivalent manuel :

```powershell
cmake --install out/build/clang-ui --prefix out/stage-windows
$stage = (Resolve-Path out/stage-windows).Path
$output = Join-Path (Get-Location) 'out/PLPT-0.1.0-preview-windows-x64-setup.exe'
& "${env:ProgramFiles(x86)}\NSIS\makensis.exe" "/DSTAGE=$stage" "/DOUTPUT=$output" packaging/windows/plpt-preview.nsi
if ($LASTEXITCODE -ne 0) { throw 'NSIS failed' }
Get-FileHash -Algorithm SHA256 $output
```

Le ZIP contient la documentation complète et les preuves. L'installateur compact
embarque seulement `QUICKSTART.md`, `DEPENDENCIES.md` et les licences SDL/ImGui.

Destination prévue : `%LOCALAPPDATA%\Programs\PLPT-Preview`. Registre **HKCU**,
raccourci de l'utilisateur courant, aucun service ni démarrage automatique,
aucune règle pare-feu et `RequestExecutionLevel user`. Une preview existante doit
être désinstallée avant remplacement. L'uninstall ne supprime que les fichiers
connus : pas de `RMDir /r` sur une racine choisie par l'utilisateur.

**État de ce livrable : recette fournie, NSIS non exécuté dans le sandbox Linux.**
Il n'existe pas ici d'installateur Windows compilé, signé et testé à annoncer.
La première release exige sur une VM Windows propre, sans outils de développement :

- installation utilisateur standard sans UAC inattendu ;
- lancement CLI/UI et vérification des dépendances DLL ;
- tests fermeture/réinstallation/désinstallation, y compris fichiers verrouillés ;
- aucun changement de pare-feu/service/autostart ;
- aucun fichier utilisateur supprimé ;
- contrôles du manifeste, signatures et empreintes.

La signature Authenticode nécessite un certificat du propriétaire, conservé hors
Git et hors chat. Ne jamais envoyer sa clé privée. Quand le certificat et la
politique de timestamp auront été choisis, signer les exécutables **avant** de
compiler/signer l'installateur. Les binaires actuels sont de développement, non
signés ; la confiance SmartScreen n'est pas garantie. La licence PLPT reste à choisir.

## 3. Linux — cible secondaire

Sur Debian/Ubuntu (prérequis **développeur**, pas runtime invité) :

```bash
sudo apt-get install clang cmake ninja-build g++
clang++ --version
./build.sh clang-debug
./out/build/clang-debug/plpt-monitor
./build.sh clang-release
./build.sh clang-asan
```

`g++` ci-dessus fournit la libstdc++ et ses headers sur ces distributions ; il ne
compile **pas** PLPT. CMake refuse un compilateur GCC pour ce projet.
Les commandes détaillées :

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug --parallel 4
ctest --preset clang-debug -V
```

L'UI Linux normale requiert les headers de développement de son backend graphique.
Exemple de socle X11 Ubuntu, à adapter à la distribution :

```bash
sudo apt-get install pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev libgl1-mesa-dev
./build.sh clang-ui
./out/build/clang-ui/plpt-desktop
```

SDL contrôle la disponibilité des drivers. Son résumé CMake doit annoncer un
backend utilisable sur la machine. Un build sans X11/Wayland n'est pas une UI
Linux livrable à un utilisateur desktop.

### Smoke test sans écran (celui réellement utilisé ici)

```bash
cmake -S . -B out/build/ui-smoke -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DPLPT_BUILD_UI=ON -DPLPT_HEADLESS_UI=ON
cmake --build out/build/ui-smoke --parallel 4
ctest --test-dir out/build/ui-smoke -V
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
  ./out/build/ui-smoke/plpt-desktop --smoke-test --screenshot out/desktop.bmp
```

Le BMP est une capture volontaire de **la fenêtre PLPT**, pas du bureau ni d'un
navigateur distant. Ce mode quitte automatiquement après initialisation et frames.

### Package Linux technique

```bash
cmake --install out/build/clang-release --prefix out/stage-linux
cpack --config out/build/clang-release/CPackConfig.cmake -B out/packages
# Vérifier seulement une archive que vous venez de construire vous-même :
python3 tools/check_linux_package.py out/packages/PLPT-0.1.0-preview-Linux-x64.tar.gz out/build/clang-release/plpt-monitor
```

Le preset Release sans UI produit une archive TGZ du **moniteur CLI**. Ni AppImage
ni `.deb` installable universel ne sont promis. La compatibilité glibc/libstdc++,
les architectures et le packaging Linux final attendent la qualification Windows.

## 4. Dépendances, offline et reproductibilité

- Sans UI : standard library + API système uniquement.
- UI : [inventaire](../third_party/DEPENDENCIES.md), pins dans `cmake/Desktop.cmake`.
- Pas de vcpkg implicite, de téléchargement `latest` ou de remplacement silencieux.
- Pour un build offline, provisionner les sources aux commits exacts et utiliser
  `FETCHCONTENT_SOURCE_DIR_SDL3` / `FETCHCONTENT_SOURCE_DIR_IMGUI`. Cela contourne le
  téléchargement/hash CMake : **vérifier soi-même les commits/hashes du miroir**.
- Ne pas réutiliser un build directory entre compilateurs, SDK/ABI ou architectures.
- CMakePresets version 5 est compatible avec CMake 3.24.

Les outils du sandbox sont installés hors Git dans `/home/user/.cache/plpt-tools`.
La configuration `PATH` effectivement utilisée est dans le rapport. Npm/Python
servent seulement à y fournir des outils de build lorsque les assets LLVM officiels
sont inaccessibles ; ils ne deviennent pas des dépendances runtime de PLPT.

## 5. Dépannage

| Symptôme | Action |
|---|---|
| `iostream`, `windows.h` ou `kernel32.lib` absent | Installer le SDK/STL ; ouvrir le terminal développeur x64 / utiliser build.bat |
| CMake trouve `cl.exe` / g++ / clang-cl | Nouveau dossier de build ; `-DCMAKE_CXX_COMPILER=clang++` |
| Mauvaise ABI Windows | LLVM pour target `x86_64-pc-windows-msvc`, pas un mélange MSYS2/MSVC |
| SDL ne trouve pas de driver graphique | Installer les headers OS ou choisir explicitement le profil headless de test |
| IPv6 indisponible | CTest marque le test `ipv6` SKIP (code 77), pas PASS ; qualification incomplète |
| PID inconnu | Droits/processus changeants ; ne pas lancer automatiquement PLPT comme administrateur |
| Téléchargement de dépendance refusé | Provisionner les archives vérifiées ; ne pas retirer `URL_HASH` |
| Installer non signé bloqué | Ne pas désactiver la protection ; qualifier et signer avant distribution |

La CI prépare une exécution Windows en premier (`windows-2022`), puis Linux avec
`needs: windows`. Son simple fichier ne constitue pas un résultat : aucun run
n'a été lancé depuis cette session, aucun push n'a été effectué.
