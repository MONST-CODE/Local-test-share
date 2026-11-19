# DevLink 0.2.0-preview — portée et démarrage local

## Fonctionnalités disponibles et limites

Cette préversion contient un moniteur local et un poste desktop natif en cours de
qualification. **Elle ne permet pas encore de partager réellement une application
web avec un invité.** Le backend WebView2 n'initialise pas de navigateur opérationnel,
ne capture pas de vrais pixels et ne reçoit pas d'interactions. Le backend géré
synthétique est réservé aux tests et à une démonstration explicitement signalée.

Le plan de contrôle TLS 1.3 PSK et le défi-réponse sont intégrés dans le code. Les
preuves existantes sont des tests d'intégration locaux ; elles ne prouvent ni une
session LAN multi-machine, ni l'authentification TLS sur Windows. La fabrique TLS
Windows est actuellement indisponible, et le moteur hôte doit refuser une session
quand TLS est requis sans runtime opérationnel. Le proxy HTTP n'est pas un flux
média et son trafic applicatif LAN n'est pas qualifié comme chiffré.

N'utilisez pas cette préversion comme service de partage de production et ne
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
- Aucun test LAN physique à deux machines, test navigateur réel ou test physique
  Windows n'est actuellement revendiqué.

Voir [`SECURITY.md`](SECURITY.md), [`NETWORK.md`](NETWORK.md) et le rapport
[`reports/FINAL_COMPLETION_AUDIT.md`](reports/FINAL_COMPLETION_AUDIT.md) avant toute
évaluation de portée.
