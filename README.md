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

Prérequis développeur : LLVM **clang++.exe** et lld-link, Visual Studio C++ Build
Tools avec **SDK Windows + STL/UCRT x64**, curl/tar/certutil natifs. Microsoft ABI
uniquement, aucun repli MinGW. Le script peut initialiser `VsDevCmd.bat`.

```bat
build.bat deps --fetch
build.bat doctor --verbose
build.bat debug
build.bat test debug --native-ui
out\windows\debug\bin\plpt-desktop.exe
build.bat release
build.bat package
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

La migration est suivie dans [DIRECT_BUILD_MIGRATION](docs/reports/DIRECT_BUILD_MIGRATION.md),
avec les résultats **réellement exécutés** séparés :
[Windows](docs/reports/BUILD_VALIDATION_WINDOWS.md) ·
[Linux](docs/reports/BUILD_VALIDATION_LINUX.md) ·
[dépendances](docs/reports/BUILD_DEPENDENCIES.md).
La **qualification P0 native Windows et Linux a réussi** au commit `13fd464` :
[exécution 43426058514](https://github.com/MONST-CODE/Local-test-share/actions/runs/43426058514),
Debug/Release, tests mémoire/fuzz, fenêtre native, nettoyage/rebuild et packages.
Cela ne vaut ni validation humaine du produit ni qualification d'une fonction de partage.
Les anciens [lots et résultats](docs/reports/VALIDATION.md) sont explicitement
historiques et ne qualifient pas la nouvelle architecture de build.

Ni le navigateur/capture, ni le transport authentifié, ni les cinq PCs, ni les
performances LAN et le nettoyage après crash ne sont qualifiés. Les packages sont
des archives de développement portables, pas des installateurs signés.

## Documentation

| Document | Contenu |
|---|---|
| [Compilation](docs/BUILD.md) | Commandes officielles, SDK/CRT, profils, sorties, diagnostics, packaging |
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

Migration sur `arena/da48a9c5-local-test-share`, depuis `main` à `73162d9`.
Aucune réécriture d'historique, modification directe de main ou fusion automatique.

La licence de distribution de PLPT reste à choisir par son propriétaire. Les
notices SDL3/ImGui sont conservées. Aucun livrable n'est une V1 LAN prête à l'emploi.
