# PLPT · Plateforme Locale de Partage et de Test

**C++20 · clang++ · Windows x64 d'abord · SDL3 + Dear ImGui**

> **Livraison 0.1.0-preview : fondations et moniteur local. Ce n'est pas encore
> l'application de partage.** Pas de connexion invité, de transport chiffré ni de
> rendu WebView2. Aucun port détecté n'est automatiquement partagé.

Le [document maître](docs/ARCHITECTURE_MASTER.md) fourni par le propriétaire est
conservé à l'identique. L'objectif reste : cinq testeurs sur le même Wi-Fi, des
contextes navigateur distincts **exécutés sur l'hôte**, sans transférer le projet.

## Ce qui est disponible

- **Moniteur CLI** `plpt-monitor` : inventaire local TCP/UDP, IPv4/IPv6, PID,
  processus, origine probable, confiance et état. Ne sonde pas le LAN.
- **Moniteur natif** `plpt-desktop` (optionnel) : filtre, actualisation par worker,
  inspection, sélection locale et marquage protégé. La sélection n'ouvre aucun accès.
- **Cœur testable** : types forts, erreurs explicites, handles/sockets RAII,
  snapshot partiel distinct d'un inventaire vide, protection contre les associations
  aveugles lors d'une réutilisation de PID.
- **Prototypes isolés** : allocation `bind(0)` avec socket conservé ; modèles locaux
  de sessions/capacités, DM bornés et enveloppes binaires validées avant allocation.
  Ces modèles ne sont **pas liés dans le moniteur** et ne constituent pas un handshake.
- **Build et validation** : CMake/Ninja, Debug/Release, sanitizers, tests natifs,
  cible de fuzzing, CI Windows prioritaire, packaging de développement.

## État vérifiable

| Périmètre | État |
|---|---|
| Lots A/B : reconnaissance, architecture et socle | Rapport (avec réserve de séquence) et source livrés ; tests Linux/clang++ exécutés |
| Lot C : API Windows TCP/UDP/processus | Code et vérification syntaxique x64 ; **exécution Windows non réalisée ici** |
| Lot D : registre, arbre et UI native | Implémentés ; build et smoke test SDL `dummy` exécutés sous Linux |
| Lot E : navigateur/capture | Interface seulement ; prototype Windows critique à réaliser |
| Lots F/G/H | Réservation OS et modèles locaux testés ; aucune connexion LAN opérationnelle |
| Lot I / V1 | **Non validés** : sécurité E2E, cinq PCs, performances, profils et nettoyage après crash |

Les commandes, sorties, erreurs corrigées et tests **non exécutés** figurent dans
le [rapport de validation](docs/reports/VALIDATION.md) et les rapports
[Lot B](docs/reports/LOT_B.md), [Lot C](docs/reports/LOT_C.md), [Lot D](docs/reports/LOT_D.md).
Une CI écrite n'est pas une
CI passée ; une vérification croisée n'est pas une exécution Windows.

## Démarrer — Windows x64

Prérequis développeur : LLVM **clang++**, Visual Studio 2022 Build Tools **C++ +
Windows SDK**, CMake ≥ 3.24, Ninja. Pas besoin de ces outils chez un futur invité.

```bat
:: Le script charge l'environnement SDK x64 si nécessaire, puis configure,
:: compile et exécute CTest. Il s'arrête sur chaque erreur.
build.bat clang-debug
out\build\clang-debug\plpt-monitor.exe

:: Interface native ; téléchargement des deux dépendances épinglées au build.
build.bat clang-ui
out\build\clang-ui\plpt-desktop.exe
```

Toutes les commandes sans script, les prérequis SDK, le packaging ZIP/NSIS,
la signature et les limitations sont dans **[docs/BUILD.md](docs/BUILD.md)**.
L'installateur NSIS reste une recette à valider sous Windows, pas un binaire
prétendument testé dans ce sandbox Linux.

### Linux — cible secondaire

```bash
./build.sh clang-debug
./out/build/clang-debug/plpt-monitor
./build.sh clang-asan
```

Pour l'UI Linux, voir les dépendances X11/Wayland dans le guide. Le build `dummy`
utilisé pour les smoke tests n'est pas une distribution desktop Linux.

## Documentation

| Document | Contenu |
|---|---|
| [Architecture technique](docs/ARCHITECTURE.md) | Modules, invariants, frontières de confiance, concurrence |
| [Compilation / installation](docs/BUILD.md) | Windows d'abord ; commandes reproductibles et installateurs |
| [Sécurité](docs/SECURITY.md) | Ce qui est protégé, ce qui reste à démontrer, aucune crypto maison |
| [Tests](docs/TESTING.md) | Suites, reproduction, critères et preuves |
| [Rapport](docs/reports/VALIDATION.md) | Résultats réels de cette livraison et limites |
| [Lot A](docs/reports/LOT_A.md) | État initial et reconnaissance |
| [ADR](docs/ADR/README.md) | Décisions validées vs décisions provisoires |
| [Protocole expérimental](docs/protocol/PROTOCOL.md) | Layout binaire, limites, absence d'authentification à ce stade |
| [Roadmap / portes](docs/ROADMAP.md) | Ordre des lots et conditions de passage |
| [Dépendances](third_party/DEPENDENCIES.md) | Versions, licences, provenance, surface d'intégration |

## Branche et distribution

Le travail reste sur `arena/9d0968ca-local-test-share`, branche imposée à cette
session. La demande de branche `dev` n'a pas été appliquée : pas de création,
renommage ou changement de branche ici. Aucun commit/push n'est nécessaire pour
conserver les fichiers dans Arena.

La licence de distribution de PLPT reste à choisir par son propriétaire. Les
licences SDL3/ImGui sont conservées séparément. Aucun package de cette livraison
ne doit être présenté comme la V1 LAN prête à l'emploi.
