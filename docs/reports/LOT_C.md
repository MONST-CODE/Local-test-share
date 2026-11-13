# Lot C — découverte native Windows, adaptateur Linux secondaire

Date : 2026-11-13. **Implémentation source + syntaxe Windows ; lot non qualifié nativement sous Windows.**

## Objectif et fichiers

Inventorier TCP/UDP IPv4/IPv6 et processus sans sondage réseau ni élévation.
`engine/platform/windows/{winsock,windows_platform}.hpp`, `windows_platform.cpp`,
`engine/platform/linux/linux_platform.{hpp,cpp}`, `engine/discovery/monitor.*`,
`tests/integration/platform_tests.cpp`, `tools/check_windows_syntax.py`.

## Décisions

PPID Windows via Toolhelp (`th32ParentProcessID`), layouts IPv4 et IPv6 distincts,
propriétaire inconnu quand l'identité de création n'est pas stable avant/après.
Tables instables bornées ; accès refusé n'entraîne pas d'élévation. Linux utilise
les inodes proc associés à la même lecture, jamais une seconde lecture positionnelle.
Les avertissements du premier snapshot processus sont conservés.

Pas de nouvelle dépendance produit. Zig/libc++/MinGW sert uniquement à une vérification
syntaxique additionnelle. Le drapeau expérimental de ce sysroot n'est pas une
modification du standard C++20 ni du build Windows Microsoft recommandé.

## Commandes, résultats et preuves

Voir [VALIDATION.md](VALIDATION.md), `tests-*.log`, `windows-syntax.log` et
`monitor-live.log` dans [evidence](evidence/2026-11-13/).

- **16 unités Windows x64** vérifiées par clang++ `-fsyntax-only`, application et tests inclus.
- **Aucun exécutable Windows lié ou exécuté** ; pas de runtime Windows dans ce sandbox.
- Linux : 6 cas plateforme / 284 checks, 2 cas IPv6 / 41 checks réussis.
- Réservations de tests par `bind(0)` et second bind concurrent refusé ; pas de port fixe.
- Inventaire réel CLI : 8 groupes dans ce sandbox ; PID inconnus laissés inconnus.

## Limites / non exécuté

Next.js/Vite/Docker Desktop/WSL2 natifs Windows, tables changeantes en charge,
processus protégés/Unicode, firewall et environnements `hidepid` réels restent
à qualifier. Le bracketing est une corrélation, pas une lecture atomique OS.
Une association terminal/projet reste une inférence, pas une identité de confiance.

## Prochaine porte

Faire tourner la CI Windows et une matrice manuelle standard-user. Ne pas considérer
le Lot C terminé sur la seule base de l'adaptateur Linux ou de headers MinGW.
