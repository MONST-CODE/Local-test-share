# PLPT · Plateforme Locale de Partage et de Test

**C++20 · clang++ · Windows x64 d'abord · SDL3 + Dear ImGui**

> **0.1.0-preview : fondations et moniteur local, pas encore une application de
> partage.** Aucun invité, transport chiffré ou WebView2 intégré. La détection
> n'active jamais de partage. La migration du build est le P0 avant toute autre fonction.

Le [document maître](docs/ARCHITECTURE_MASTER.md) reste conservé à l'identique.
L'objectif : cinq testeurs sur le même Wi-Fi, des contextes navigateur distincts
**exécutés sur l'hôte**, sans transmettre le projet.

## Disponible dans le code

- **CLI `plpt-monitor`** : inventaire local TCP/UDP, IPv4/IPv6, PID et origine
  probable. Aucun scan LAN ou serveur de partage.
- **UI native `plpt-desktop`** : filtre, worker d'actualisation, inspection et
  sélection locale. Cette sélection ne donne aucun accès réseau.
- **Cœur C++20** : erreurs explicites, sockets/handles RAII, inventaire incomplet
  distinct d'une liste vide, protection contre les associations aveugles de PID.
- **Modèles expérimentaux testés séparément** : session/rôles, messages bornés,
  enveloppe binaire. Ils ne sont pas liés dans le moniteur et ne sont pas un handshake.
- **Build direct** : seuls **`build.bat` et `build.sh`** définissent les sources,
  dépendances, flags, compilations, éditions de liens, tests et packages. Aucun
  générateur/moteur externe ni orchestration Python/PowerShell.

## Compiler — Windows x64 prioritaire

Prérequis développeur : **MSYS2 UCRT64** avec ses paquets mingw-w64 (clang++, binutils,
SDL3). Le compilateur est exclusivement
`C:\msys64\ucrt64\bin\clang++.exe`, cible **x86_64-w64-windows-gnu**, UCRT et GNU
libstdc++. Pas de Visual Studio, SDK Microsoft, LLVM autonome, CLANG64 ni ABI MSVC ;
une autre racine MSYS2 passe uniquement par `PLPT_MSYS2_ROOT`. Votre copie ImGui
existante peut être utilisée via `PLPT_IMGUI_DIR` après vérification du pin.

```bat
./build.bat deps --fetch
./build.bat doctor --verbose
./build.bat debug
./build.bat test debug --native-ui
./out\windows\debug\bin\plpt-desktop.exe
./build.bat release
./build.bat package
```

Les téléchargements sont **explicites, épinglés et vérifiés par SHA-256**. Les
opérations de compilation ne téléchargent rien. Chaque build repart d'objets propres.

### Linux — cible secondaire

Installer LLVM clang/clang++, libstdc++, libx11-dev, libxext-dev et les utilitaires
natifs détaillés dans le guide, puis :

```sh
./build.sh deps --fetch
./build.sh doctor
./build.sh debug
./build.sh test debug --native-ui
./out/linux/debug/bin/plpt-desktop
./build.sh test asan --native-ui
./build.sh package --native-ui
```

`--headless` est un profil SDL dummy explicite pour sandbox sans affichage ; il ne
qualifie pas un bureau natif et ne peut pas produire un package desktop.

## Preuves et limites

La correction Windows est suivie par
[UCRT64_TOOLCHAIN_MIGRATION](docs/reports/UCRT64_TOOLCHAIN_MIGRATION.md),
[UCRT64_WINDOWS_VALIDATION](docs/reports/UCRT64_WINDOWS_VALIDATION.md) et
[UCRT64_DEPENDENCIES](docs/reports/UCRT64_DEPENDENCIES.md), avec les résultats
**réellement exécutés**. Les pages [Windows](docs/reports/BUILD_VALIDATION_WINDOWS.md)
et [dépendances](docs/reports/BUILD_DEPENDENCIES.md) antérieures sont conservées
comme preuve de l'ancien profil Microsoft, **explicitement remplacé** ; elles ne
qualifient pas UCRT64. [Linux](docs/reports/BUILD_VALIDATION_LINUX.md) reste distinct.
Cela ne vaut ni validation humaine du produit ni qualification d'une fonction de partage.
Les anciens [lots et résultats](docs/reports/VALIDATION.md) sont explicitement
historiques et ne qualifient pas la nouvelle architecture de build.

Ni le navigateur/capture, ni le transport authentifié, ni les cinq PCs, ni les
performances LAN et le nettoyage après crash ne sont qualifiés. Les packages sont
des archives de développement portables, pas des installateurs signés.

## Documentation

| Document | Contenu |
|---|---|
| [Compilation](docs/BUILD.md) | UCRT64 exact, ImGui/SDL du propriétaire, VS Code, profils et sorties |
| [Utilisation](docs/QUICKSTART.md) | Installation portable et limites de la preview |
| [Architecture](docs/ARCHITECTURE.md) | Modules, invariants, frontières de confiance |
| [Sécurité](docs/SECURITY.md) | Protection actuelle et preuves encore nécessaires |
| [Tests](docs/TESTING.md) | Suites, codes retour, sanitizers, fuzz et tests négatifs |
| [Migration P0](docs/reports/DIRECT_BUILD_MIGRATION.md) | Reconnaissance, décisions, preuves et limites |
| [ADR](docs/ADR/README.md) | Décisions validées ou provisoires |
| [Protocole expérimental](docs/protocol/PROTOCOL.md) | Layout borné, sans authentification à ce stade |
| [Roadmap](docs/ROADMAP.md) | Lots et conditions de passage |
| [Dépendances](third_party/DEPENDENCIES.md) | Versions, licences, provenance, mise à jour |

## Branche et distribution

Correction UCRT64 sur `arena/4197f0b0-local-test-share`, depuis `main` à `3b62b48`.
Aucune réécriture d'historique, modification directe de main ou fusion automatique.

La licence de distribution de PLPT reste à choisir par son propriétaire. Les
notices SDL3/ImGui sont conservées. Aucun livrable n'est une V1 LAN prête à l'emploi.
