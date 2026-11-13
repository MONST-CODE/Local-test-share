# Inventaire des dépendances — 2026-11-13

Aucun service tiers de tunnel, stockage, authentification ou télémétrie. Aucune
bibliothèque de crypto ajoutée pour simuler un transport. Pas de téléchargement
au lancement de PLPT. Les téléchargements ci-dessous sont **de build**, côté développeur.

## Bibliothèques du produit

| Nom | Version / identité fixée | Licence / origine | Raison et surface |
|---|---|---|---|
| Standard C++20 + API OS | Toolchain/SDK documentés dans le rapport de test | LLVM / Microsoft SDK ; libc++ ou STL MSVC / libstdc++ selon cible | Types, threads, allocations ; Win32 Iphlpapi/Toolhelp32/Winsock, BCryptGenRandom ; Linux proc/getrandom/getifaddrs |
| SDL3 (option UI) | 3.4.20, `f00dc919ca35b9756316e86c4777392de442f9b2` | zlib, https://github.com/libsdl-org/SDL | Fenêtre, événements, rendu SDL_Renderer. Accès aux périphériques/drivers ; pas de socket PLPT. Audio/joystick/haptic désactivés dans ce moniteur. |
| Dear ImGui (option UI) | 1.93.0, `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | MIT, https://github.com/ocornut/imgui | UI et backends officiels SDL3/SDLRenderer3. Pas de réseau. INI/logging désactivés pour la preview. |

Archives codeload épinglées par commit et SHA-256 dans `cmake/Desktop.cmake` :

- SDL3 : `e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0`
- ImGui : `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114`

**Régression SDL détectée par nos tests :** dans cette révision, `SDL_SaveBMP`
alloue une surface intermédiaire avant d'ouvrir le fichier et ne la libère pas si
l'ouverture échoue (`src/video/SDL_bmp.c`, fonction `SDL_SaveBMP`). LeakSanitizer a
signalé 3 780 400 octets retenus avec notre capture 1400×900. Aucun patch tiers ni
suppression de diagnostic : PLPT ouvre un `SDL_IOStream` explicitement et utilise
`SDL_SaveBMP_IO(..., false)`, avec propriété RAII et fermeture vérifiée. Les tests
conservent les chemins « fichier impossible » et « disque plein » (Linux). Rejouer
ces régressions à chaque mise à jour SDL avant de simplifier l'adaptateur.

Les licences originales sont copiées dans `third_party/licenses/` et installées
avec le moniteur UI. Le code tiers n'est pas commité ni modifié. Le cœur sans UI
ne télécharge rien. `FetchContent` est exécuté seulement si `PLPT_BUILD_UI=ON`.

## Composants embarqués et bibliothèques système

Les archives épinglées contiennent aussi des composants en source : aucune
récupération supplémentaire ni suivi de leur branche amont n'est effectué.
`licenses/Bundled-notices.txt` est installé avec l'UI et inclus dans la recette NSIS.

| Composant | Identité / licence | Surface et mise à jour |
|---|---|---|
| ImGui imstb_truetype / rectpack / textedit | 1.26 / 1.01 / 1.14, variantes du commit ImGui ; alternative MIT retenue (Sean Barrett) | Rasterisation des fontes, atlas, saisie texte ; ne pas accepter de fontes invitées ; régression UI après mise à jour ImGui |
| ProggyClean / ProggyForever subset | Données incorporées au commit ImGui ; MIT, Tristan Grimmer / Disco Hello | Fontes de l'UI, aucun fichier distant téléchargé ; notices conservées |
| Plages de glyphes japonais ImGui | Données au même commit ; attribution CC BY 4.0 des listes officielles japonaises conservée | Tables compilées ; pas une garantie de couverture des glyphes de notre fonte |
| SDL stb_image | 2.30, inclus dans le commit SDL ; alternative MIT retenue | Décodeur PNG/JPEG embarqué par SDL ; PLPT ne lui soumet aucune image invitée dans ce moniteur |
| SDL yuv2rgb | Révision du commit SDL ; BSD-3-Clause, Adrien Descamps | Conversion de surfaces ; notices conservées, tests SDL lors de mise à jour |
| SDL qsort / libm / fallbacks | qsort 1.16 zlib ; notices fdlibm Sun ; dlmalloc 2.8.6 et PDClib CC0, tous figés via SDL | Primitives de la bibliothèque ; pas de fork PLPT ; notices pertinentes conservées |

HIDAPI/libusb et l'API SDL_GPU sont **explicitement désactivés** dans ce moniteur,
en plus d'audio/joystick/haptic/sensor/camera. SDLRenderer3 reste le backend UI ;
le choix du renderer vidéo final n'est pas figé. L'audit des DLL et notices de
chaque futur package Windows reste une condition de distribution, pas un résultat acquis.

Sous Linux, `ldd` montre libstdc++, libgcc_s, libm et glibc du système. Libstdc++/
libgcc relèvent de GPL-3.0-or-later avec GCC Runtime Library Exception 3.1 ; glibc
(et libm) de LGPL-2.1-or-later. Elles ne sont pas copiées dans notre TGZ CLI.
Le libc++ du cross-check est Apache-2.0 WITH LLVM-exception. Sur Windows, le code
STL Microsoft est MIT, mais SDK/CRT et leur redistribution relèvent aussi des
conditions des Visual Studio Build Tools/Microsoft : vérifier les droits et notices
du toolset exact avant de distribuer un binaire à runtime statique. Aucun SDK/CRT
Microsoft n'a été téléchargé ou redistribué par cette session.

## Outils de développement (pas requis chez l'invité)

- clang++ : LLVM, Apache-2.0 WITH LLVM-exception. Compilateur imposé par CMake.
- CMake 3.31.6 employé ici (minimum 3.24), BSD-3-Clause, distribution PyPI `cmake`.
- Ninja : package PyPI `ninja==1.11.1.4`, Apache-2.0 ; le binaire affiche
  `1.11.1.git.kitware.jobserver-1`.
- NSIS 3.11 : licence zlib/libpng pour l'outil et licences propres aux composants ;
  utilisé uniquement pour l'installateur Windows à valider. https://github.com/NSIS-Dev/nsis
- Harness de tests : code du dépôt, aucune bibliothèque de test tierce.
- libFuzzer / ASan / UBSan : outils LLVM, non liés dans un package Release.

### Provisionnement spécifique au sandbox

Les assets de releases GitHub redirigent vers un hôte inaccessible ici. Le
clang++ Linux utilisé provient des packages npm `llvm-runtime-clang-linux-x64`,
`llvm-runtime-lld-linux-x64`, `llvm-runtime-resources-linux` **22.1.8**, dépôt
https://github.com/holepunchto/llvm-runtime (Apache-2.0 / LLVM-exception).
Installation avec `--ignore-scripts`, intégrité npm verrouillée dans le cache hors
Git. Ce canal secondaire de distribution augmente le risque supply-chain :
ce n'est pas le canal recommandé pour la release Windows. Le rapport donne
la version réellement affichée et la commande ; utiliser LLVM officiel et le
SDK Microsoft sur Windows.

`ziglang` **0.14.1**, PyPI (MIT / licences LLVM et MinGW incluses) fournit seulement
les headers Windows/libc++ pour une éventuelle vérification syntaxique croisée.
Il ne remplace pas clang++, le SDK Microsoft ni l'exécution des tests Windows.
Son libc++ 19 exige `-fexperimental-library` pour déclarer `std::jthread` /
`std::stop_token` : drapeau réservé au script syntaxique, pas à la build Windows
Microsoft. Aucune bibliothèque expérimentale n'est liée par ce script.

Pillow **11.3.0**, licence **MIT-CMU** (et notices des codecs dans sa distribution
PyPI), est utilisé hors dépôt uniquement pour convertir la capture BMP générée
localement en PNG pour le rapport. Il n'est ni lié ni distribué avec PLPT et ne
traite pas d'images reçues d'invités. Ne pas en faire un décodeur réseau implicite.
Lors d'une nouvelle capture, vérifier la version/licence et les avis de sécurité
comme pour tout outil traitant des images.

## Dépendances non retenues à ce stade

WebView2, QUIC/MsQuic, encodeur/décodeur, DNS-SD, Docker : aucune intégration ni
version fictivement validée. Le choix doit contenir version, licence, mises à jour,
API/socket ownership et tests de sécurité dans un ADR avant ajout.

## Mise à jour

1. Lire changelog, avis de sécurité et licence à la source ; ne pas suivre `main`.
2. Actualiser le commit + le hash + ce fichier + les licences.
3. Refaire Debug/Release, ASan/UBSan, tests Windows et UI (DPI, fermeture, erreurs).
4. Inspecter les DLL réellement chargées et le package. Aucune dépendance
   additionnelle ou transitive non inventoriée ne doit être livrée silencieusement.

Le propriétaire n'a pas encore choisi de licence de distribution pour **PLPT**.
Aucune licence open-source n'est imposée au code du projet par cet inventaire.
