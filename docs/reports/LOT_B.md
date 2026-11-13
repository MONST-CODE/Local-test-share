> **HISTORICAL / SUPERSEDED:** this report describes the original CMake-based preview, not the current build architecture. See [DIRECT_BUILD_MIGRATION.md](DIRECT_BUILD_MIGRATION.md) and the native platform reports. Original logs remain historical evidence.

# Lot B — socle C++20 et contrats

Date : 2026-11-13. **Source livrée, qualification Linux exécutée ; Windows native en attente.**

## Objectif et fichiers

Fournir des types distincts, erreurs explicites, horloge injectable, entropie OS,
interfaces séparées et une chaîne clang++/CMake. Fichiers : `CMakeLists.txt`,
`CMakePresets.json`, `engine/core/`, `engine/platform/platform.hpp`,
`engine/platform/native.hpp`, `engine/diagnostics/`, `build.bat`, `build.sh`,
`tests/test_harness.hpp`, `tests/unit/`, `tools/check_headers.py`.

Les interfaces navigateur, transport et stockage refusent les fonctionnalités
absentes ; aucun transport fictif n'est une preuve d'authentification. Les modèles
anticipés de session/chat/codec restent séparés des exécutables installés.

## Décisions et dépendances

ADR-001/002 et [ADR-011](../ADR/ADR-011-proof-gates.md). Pas de dépendance runtime
hors bibliothèque C++/API OS pour le cœur. L'inventaire des **outils** explique
la provenance particulière du clang++ du sandbox. Aucun service tiers produit ajouté.

## Commandes et résultats

Les commandes exactes, versions, SHA et sorties sont centralisés dans
[VALIDATION.md](VALIDATION.md) pour éviter des chiffres divergents entre lots.
Exécutés : Debug, Release, ASan+UBSan, checker de **20 headers indépendants**, refus
de GCC et arguments CLI. Résultat actuel du cœur : **5/5 entrées CTest** dans chacune
des trois configurations Linux. Toutes les vérifications restent actives en Release.

## Limites et prochaine porte

Une assertion de type n'authentifie pas un identifiant fourni par le réseau.
L'entropie OS ne constitue pas un protocole d'invitation. Pas de validation ABI/SDK
Microsoft ni exécution Windows ; elles sont nécessaires avant qualification de
la cible prioritaire. Prochaine porte : build natif Windows du socle et du Lot C,
en conservant les mêmes tests négatifs.
