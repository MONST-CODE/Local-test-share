# ADR-011 — Livraison par preuves et modèles non exposés

Date : 2026-11-13. Statut : décision d'implémentation de cet incrément,
conforme aux principes §0.4/§26 du maître ; ne vaut pas acceptation V1.

## Contexte

Le dépôt initial était vide. Le premier socle écrit n'avait pas encore été compilé,
mais son README marquait plusieurs lots « faits/testés ». La revue a aussi identifié
des raccourcis incompatibles avec la cible : empreinte FNV d'invitation, PPID Windows
incorrect, layouts IPv6 castés comme IPv4 et sweep récursif de dossiers par préfixe.

## Options

1. Conserver toutes les promesses et masquer les adaptateurs absents par des mocks.
2. Réduire à un simple document sans chemin exécutable.
3. Livrer un moniteur réel avec tests, isoler les modèles anticipés, refuser les
   opérations non qualifiées et documenter les portes encore ouvertes.

## Décision

Option 3. Séparer `plpt_core` / applications et `plpt_models` de test. Aucun faux
transport sécurisé. Retirer l'empreinte FNV et le sweep dangereux plutôt que les
présenter comme une sécurité provisoire. Les interfaces navigateur/stockage échouent
explicitement. Le rapport distingue compilation, syntaxe croisée, exécution native,
smoke test, contrôle manuel et non-exécution.

Le sandbox Linux ne change pas la priorité Windows : les gates exigent des
résultats Windows natifs, jamais une substitution par une syntaxe croisée.

**Mise à jour P0 du 13 novembre 2026 :** cette preuve de build/tests natifs existe
maintenant via `build.bat` (MSYS2 UCRT64 clang++ uniquement, profil Microsoft remplacé)
et `build.sh`, avec rapports
[Windows](../reports/BUILD_VALIDATION_WINDOWS.md), [Linux](../reports/BUILD_VALIDATION_LINUX.md)
et [migration](../reports/DIRECT_BUILD_MIGRATION.md). Aucune release automatique.
Les groupes cœur/modèles sont des listes de sources explicites dans ces deux scripts,
non des bibliothèques artificielles ou un moteur de build intermédiaire.

## Conséquences / risques

Le produit ne partage encore rien. Cela limite le périmètre démontrable mais évite
l'ouverture d'un chemin non authentifié. Les modèles pourront être réutilisés,
pas simplement branchés sur un socket : un orchestrateur authentifié et une revue
seront obligatoires. Effort de validation Windows et prototype WebView2 restent
le principal coût non levé.

## Preuves requises

- Compilation clang++ stricte, Debug/Release, tests d'erreurs et sanitizers.
- Pas de lien de `plpt_models` dans les exécutables distribués.
- Révocation, expiration exacte, contexte, verrou et DM testés localement.
- Rapport sans déclarations de chiffrement, capture ou nettoyage non exécutés.
