# DevLink 0.2.0-preview — portée et démarrage local

## Fonctionnalités disponibles et limites

Sous **Windows 10/11 x64**, DevLink partage réellement une application web locale :
l'hôte l'exécute dans un navigateur WebView2 isolé, chaque invité reçoit ses images
par un canal TLS 1.3 qui lui est propre et la pilote selon le mode A/B/C, que l'hôte
fait respecter (ADR-018). Le parcours est décrit dans [`USER_GUIDE.md`](USER_GUIDE.md).

Limites exactes : qualifié sur **une machine** (vrais exécutables hôte et invité).
Le test sur deux ordinateurs d'un même LAN n'est **pas encore exécuté**
([`reports/REAL_LAN_QUALIFICATION.md`](reports/REAL_LAN_QUALIFICATION.md)). Pas
d'invité Linux pour l'instant. Le proxy HTTP ne fait pas partie du partage. Ne
l'exposez pas à un réseau non fiable.

## Préparer une build

Les commandes officielles et prérequis sont dans [`BUILD.md`](BUILD.md).

Linux x64 (toolchain officielle clang++ >=18) :

```sh
./build.sh help
./build.sh deps --fetch
./build.sh doctor --verbose
./build.sh debug
./build.sh test debug --headless
```

Le profil headless vérifie le rendu dummy seulement. Il ne prouve pas l'ouverture
d'une fenêtre X11/Wayland. Dans l'environnement de continuation, Clang++ et les
headers X11 n'étaient pas présents ; les anciennes mesures GCC sont des résultats
exploratoires, pas une validation officielle actuelle.

Windows x64 : utiliser un terminal Windows natif avec la toolchain MSYS2 UCRT64
Clang++ prescrite dans `BUILD.md`, puis `build.bat help` et les commandes indiquées
par ce script. La build Windows, WebView2, TLS Windows et le package natif doivent
être testés sur Windows ; ils ne sont pas validés par les commandes Linux.

## Inspecter les services

L'outil `devlink-monitor` sert au diagnostic local et à la découverte informative.
Un processus/port détecté n'est jamais automatiquement autorisé, publié ou partagé.
Utilisez `devlink-monitor --help` pour les options réellement disponibles dans la
build locale. Ne lancez pas l'application en administrateur pour contourner une
limite de visibilité sans en comprendre la cause.

## État des preuves

- Les suites et le selftest GCC exploratoires sont rapportés dans
  `docs/reports/evidence/2026-11-18-continuation-audit.txt` ; ils ne remplacent pas
  Clang++ officiel.
- Les captures DPI logicielles sont dans `docs/reports/evidence/dpi-captures/` ;
  elles ne prouvent pas le DPI physique multi-moniteurs.
- Navigateur réel, partage et bout en bout sur les vrais exécutables : suites
  `devlink-browser-tests`, `devlink-share-tests`, `devlink-desktop-e2e-tests` de
  `build.bat test` (Windows natif). La capture de la fenêtre invitée est dans
  `docs/reports/evidence/2026-11-21-desktop-e2e-guest-window.png`.
- Aucun test LAN physique à deux machines n'est revendiqué.

Voir [`SECURITY.md`](SECURITY.md), [`NETWORK.md`](NETWORK.md) et le rapport
[`reports/FINAL_COMPLETION_AUDIT.md`](reports/FINAL_COMPLETION_AUDIT.md) avant toute
évaluation de portée.
