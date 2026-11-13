# Inventaire des dépendances — build direct, 2026-11-13

Aucun service tiers de tunnel, stockage, authentification ou télémétrie. Aucune
crypto inventée. Aucun téléchargement au lancement de PLPT. Seul `deps --fetch`,
explicitement demandé par le développeur, peut télécharger les archives épinglées.

## Bibliothèques du produit

| Nom | Version / identité | Licence / origine | Raison et surface |
|---|---|---|---|
| C++20 + API OS | Clang UCRT64 exact et versions runtime mesurés dans le rapport Windows UCRT64 ; clang++ natif/glibc/libstdc++ Linux | Apache-2.0 WITH LLVM-exception, MinGW-w64/UCRT, GNU libstdc++ | Types/threads ; Iphlpapi/Toolhelp32/Winsock/BCrypt ; proc/getrandom/getifaddrs |
| SDL3 | 3.4.20 ; Windows : paquet MSYS2 UCRT64 `mingw-w64-ucrt-x86_64-sdl3` **3.4.20-1**, archive SHA-256 `4a4438…f78` ; Linux : source `f00dc919ca35b9756316e86c4777392de442f9b2` | zlib, https://github.com/libsdl-org/SDL + notices MSYS2 | Fenêtre, événements, SDL_Renderer. Windows : import GNU `libSDL3.dll.a` + DLL UCRT64 vérifiés ; Linux : profil direct-source X11/software, fonctionnalités périphériques désactivées/stubs. |
| Dear ImGui | 1.93.0, `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | MIT, https://github.com/ocornut/imgui | UI, backends officiels **SDL3 + SDLRenderer3 uniquement**. Aucun réseau. INI/logging désactivés. |

Les URL, hashes, chemins, ABI et contraintes runtime complets sont dans
[UCRT64_DEPENDENCIES](../docs/reports/UCRT64_DEPENDENCIES.md), et l'ancien profil
Microsoft dans [BUILD_DEPENDENCIES](../docs/reports/BUILD_DEPENDENCIES.md) (historique,
remplacé). Les identités et intégrations réelles appartiennent uniquement à
`build.bat` / `build.sh` :

- Windows SDL UCRT64 `mingw-w64-ucrt-x86_64-sdl3-3.4.20-1-any.pkg.tar.zst` SHA-256 : `4a44381560c433ff42da3a6aff788fba2bba84a44b7f63ea396dec9c1cf79f78`.
- Windows ImGui 1.93.0 archive SHA-256 : `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114`.
- Linux SDL source : `e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0`.
- ImGui source, deux OS : `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114`.

Chaque build vérifie de nouveau les archives puis les extrait dans un dossier
neuf. Pas de sources tierces commitées, de modification cachée d'une extraction,
de branche flottante ni de build amont lancé indirectement. Les licences originales
et `Bundled-notices.txt` sont conservés et livrés avec les packages.

**Régression SDL à préserver :** `SDL_SaveBMP` de cette révision alloue une surface
avant d'ouvrir le fichier et la fuit si l'ouverture échoue. Le test historique
LSan a mesuré 3 780 400 octets. PLPT ouvre donc un `SDL_IOStream` explicitement,
utilise `SDL_SaveBMP_IO(..., false)`, gère la surface par RAII et vérifie la fermeture.
Aucun patch tiers ou suppression sanitizer. Rejouer « fichier impossible » et
« disque plein » (Linux) avant de simplifier cet adaptateur.

**Deuxième régression observée sur le vrai backend X11 :** SDL 3.4.20 oublie de
libérer `XTextProperty.value` lorsque `XmbTextListToTextProperty(..., XTextStyle, ...)`
retourne `conv > 0`. Notre titre contenant un tiret cadratin a produit une fuite
LSan de **37 octets** (pile native conservée). Le titre de la fenêtre OS est désormais
ASCII, garanti par `static_assert`, donc représentable sans cette branche d'erreur.
Le contenu français/Unicode reste rendu par ImGui à l'intérieur. Aucun flag de
sanitizer désactivé, aucun patch silencieux de SDL. Revalider le chemin de titre
X11 et cette précaution lors de la prochaine mise à jour du pin SDL.

## Notices incorporées

| Composant | Identité / licence | Surface et règle |
|---|---|---|
| ImGui imstb_truetype / rectpack / textedit | 1.26 / 1.01 / 1.14, variantes du commit ImGui ; alternative MIT (Sean Barrett) | Fontes, atlas et saisie ; pas de fontes invitées |
| ProggyClean / ProggyForever subset | Données du commit ImGui ; MIT, Tristan Grimmer / Disco Hello | Fontes locales, aucun téléchargement |
| Plages de glyphes japonais ImGui | Même commit ; attribution CC BY 4.0 conservée | Tables, pas une garantie de couverture de la fonte |
| SDL stb_image | 2.30 via commit SDL ; alternative MIT | Décodeur PNG/JPEG ; aucune image invitée soumise dans cette preview |
| SDL yuv2rgb | Commit SDL ; BSD-3-Clause, Adrien Descamps | Conversion de surfaces |
| SDL qsort / libm / fallbacks | qsort 1.16 zlib ; fdlibm Sun ; dlmalloc 2.8.6, PDClib CC0 via SDL | Primitives ; notices conservées, pas de fork PLPT |
| SDL X11 EDID et xsettings | Sources du commit SDL ; permissions Red Hat 2007 et Red Hat 2001/2007 + Igalia 2024, notices intégrales conservées | Description d'écran et réglages X11 ; ajoutés au profil Linux natif, aucune source flottante |
| HIDAPI Windows | Snapshot 0.14.0 inclus dans SDL ; alternative BSD retenue | Surface du binaire SDL officiel plus large que le profil Linux ; pas d'initialisation HID par PLPT |
| Valve OpenVR / Khronos | Headers inclus dans SDL ; Valve BSD-3-Clause, Khronos MIT/Apache-2.0, Vulkan 1.3.282 | Notices conservatrices pour la DLL officielle ; aucun runtime VR ajouté, aucun backend ImGui supplémentaire |

Le profil **Linux** désactive/stube audio, joystick, haptic, sensor, camera, HIDAPI,
GPU, lancement de processus, dialogues et tray. Ce n'est pas le profil compilé de
la DLL **officielle Windows**, dont la surface est plus large : l'application
n'initialise que la vidéo. Ne pas prétendre avoir désactivé une fonction dans ce
binaire tiers. Le renderer vidéo de la future application de partage reste à choisir.

## ABI et redistribution

Windows : **clang++.exe UCRT64 exclusif**, cible x86_64-w64-windows-gnu, import
GNU/libSDL3.dll.a et MinGW-w64/UCRT/GNU libstdc++. Aucune DLL Microsoft C/C++ n'est
livrée. Le build inspecte récursivement les imports PE, copie uniquement des DLL
issues du préfixe UCRT64 sélectionné avec version/SHA-256/notices, et exécute
l'archive extraite avec un PATH système seul. libstdc++/libgcc relèvent de GPL avec
Runtime Library Exception, libiconv (si importé) de LGPL, SDL de zlib. Voir le
rapport UCRT64 réel pour la liste/versions observées ; aucune autonomie universelle
ni approbation juridique n'est prétendue.

Linux : SDL partagée livrée à côté du binaire ; glibc/libm (LGPL-2.1-or-later),
libstdc++/libgcc_s (GPL avec GCC Runtime Library Exception 3.1), X11/Xext et leurs
bibliothèques système ne sont pas copiées. L'archive requiert l'ABI de sa plateforme.

## Outils de développement seulement

- MSYS2 UCRT64 clang++, binutils (windres/objdump/ld), compiler-rt ASan/libFuzzer :
  Apache-2.0 WITH LLVM-exception / GPL pour GNU binutils, selon paquet.
- GNU libstdc++/libgcc runtime fournis par le toolchain UCRT64 (RLE) ; MinGW-w64 UCRT
  headers/imports. Aucun SDK, Build Tools, rc/mt/dumpbin ou lld-link Microsoft.
- Bash/cmd, curl, tar et utilitaires natifs SHA/copie/comparaison ; Xvfb comme
  environnement d'affichage CI Linux, pas comme moteur de build.
- Harness de tests : code du dépôt, aucune bibliothèque tierce de tests.
- Aucun Jenga, CMake, Ninja, Make, MSBuild, CTest, CPack, Python ou PowerShell
  pour orchestrer la compilation, les tests ou les packages.

Le sandbox Linux utilise les packages npm `llvm-runtime-{clang-linux-x64,
resources-linux,lld-linux-x64}` **22.1.8**, projet
https://github.com/holepunchto/llvm-runtime . SHA-512 vérifiés avec les métadonnées
registry, extraction sans scripts de cycle de vie. Canal secondaire de distribution :
limite supply-chain explicite, pas un substitut aux tests Windows UCRT64 natifs.
Les anciens outils CMake/Ninja/Zig/Pillow restent seulement dans les rapports historiques.

## Non retenu et politique de mise à jour

WebView2, QUIC/MsQuic, encodeur/décodeur, DNS-SD, Docker : aucune intégration ou
version fictivement validée. Tout ajout exige version/licence/ADR, frontières API,
propriété des sockets et tests appropriés.

1. Revoir changelog, avis de sécurité et licences à la source ; jamais suivre `main`.
2. Mettre à jour commit/archive/hash, documentation et notices ensemble.
3. Rejouer clean Debug/Release, tests natifs Windows/Linux, headers, sanitizers,
   fuzz, UI (fermeture/erreurs/capture) et extraction/exécution du package.
4. Auditer imports et notices transitives ; aucune DLL nouvelle livrée silencieusement.

La licence du code PLPT reste une décision du propriétaire, indépendante de cet inventaire.
