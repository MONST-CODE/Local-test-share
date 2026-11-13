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

Le sandbox Linux ne change pas la priorité Windows. Il permet de vérifier les
invariants portables et l'adaptateur secondaire, mais **les gates du maître exigent
encore des résultats natifs Windows**. Une CI Windows est fournie sans badge de succès
inventé ni publication automatique de release.

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
