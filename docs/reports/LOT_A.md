# Lot A — Reconnaissance et décision de périmètre

Date : 2026-11-13 (UTC). Référence : document maître v1.0, §26.1–26.3.

## État observé avant le socle

La reconnaissance initiale, en lecture seule, a trouvé un dépôt ne contenant que
`README.md` (`# Local-test-share`), un commit `8845b2dd05e38321fcb78ddd6d42cb3d597788f7`
et aucun build, dépendance, test, installateur ou application. Branche de session :
`arena/9d0968ca-local-test-share`. Cette branche est imposée à la session ; la demande
`dev` ne peut pas être appliquée ici. Aucun changement de branche n'est effectué.

Commandes de reconnaissance initiale : `pwd`, `ls -la`, `git status`,
`git branch -a`, `git log --oneline -5`, inventaire des fichiers.

La vérification de reprise a trouvé des fichiers de socle **non compilés** sous
`engine/` et `tests/`. Les indications « fait/testé » du premier README étaient
prématurées : elles ne constituent pas une preuve et sont remplacées par une
matrice de validation. La reconnaissance initiale a précédé le code ; son rapport
formel n'avait pas encore été écrit. Ce document enregistre ce fait sans réécrire
l'historique du travail.

## Environnement constaté

- Linux x86_64 Debian, noyau `6.1.158+`, Python 3.11.2.
- `g++` 12.2.0 et `make` présents ; **clang++, CMake, Ninja, SDL3 absents** initialement.
- Pas de Windows interactif, SDK Microsoft installé, GPU Windows, WebView2, ni
  cinq machines Wi-Fi accessibles. Une compilation croisée ne remplacera pas ces tests.
- Réseau sortant restreint. Les outils de build seront installés hors Git dans
  un cache ; leurs versions et commandes seront enregistrées.

## Décisions de travail

1. Préserver le document maître à l'identique dans `docs/ARCHITECTURE_MASTER.md`.
2. Priorité Windows x64 / clang++ / API documentées. Linux sert aussi à tester les
   invariants portables dans cet environnement, sans déclarer Windows validé.
3. Livrer un moniteur local exploitable et des fondations vérifiées avant le
   partage réel. Les lots E à I ne sont pas déclarés achevés par des interfaces
   ou des tests en mémoire.
4. Ne pas ouvrir de service réseau de démonstration ni transmettre un projet.
5. Bloquer les intégrations dangereuses : pas de crypto artisanale, pas de
   nettoyage récursif « par préfixe », aucune autorisation héritée d'un PID réutilisé.
6. Compiler, tester les erreurs et documenter les limites avant de présenter
   les résultats. Pas de performance ni de nettoyage annoncés sans preuve.

## Risques prioritaires et portes

| Risque | Porte avant intégration |
|---|---|
| Capture WebView2 masquée/minimisée | Prototype Windows instrumenté (Lot E) |
| Mauvaise association endpoint/PID | Identité de création, incertitude visible, tests négatifs |
| Droits encore actifs après clôture | Machine d'état et refus côté moteur |
| Pile QUIC non choisie | ADR dépendance, TLS, identité hôte, refus 0-RTT sensible |
| Effacement de profils encore utilisés | Lease/identité de processus + audit natif ; aucun sweep aveugle |
| Installateur prématuré | Package de développement étiqueté, pas de promesse V1 |

Prochain travail : Lot B compilable, Lot C Windows, portion registre/moniteur du
Lot D. Lots F–H : modèles locaux expérimentaux uniquement, explicitement isolés
et sans canal d'authentification. Le détail des résultats sera dans `VALIDATION.md`.
