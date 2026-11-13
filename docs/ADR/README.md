# Registre des décisions d'architecture

La source normative des décisions 001–010 reste le [maître §24](../ARCHITECTURE_MASTER.md#24-décisions-darchitecture-adr-initiales).
Les statuts « validé » ci-dessous sont ceux du propriétaire, pas une certification
par les tests de cette branche.

| ADR | Décision | Statut du maître / état d'implémentation |
|---|---|---|
| 001 | C++20, SDL3, Dear ImGui | VALIDÉ ; moniteur natif livré |
| 002 | Windows x64 avant Linux | VALIDÉ ; CI Windows prioritaire, runtime Windows non testé ici |
| 003 | Aucun tunnel tiers | VALIDÉ ; aucun service de ce type ajouté |
| 004 | Rendu exécuté sur l'hôte | VALIDÉ priorité ; backend navigateur absent |
| 005 | Découverte ports/processus avec confiance | VALIDÉ ; code et tests de socle |
| 006 | Données sur ports OS dynamiques | RECOMMANDÉ ; prototype réservation, pas de transport |
| 007 | QUIC/TLS via pile maintenue | RECOMMANDÉ ; pile/version/licence à choisir et prouver |
| 008 | Messagerie mémoire, hôte endpoint | VALIDÉ ; modèle local, pas de messagerie chiffrée opérationnelle |
| 009 | Multi-invités | VALIDÉ ; modèle de capacité testé, pas cinq machines connectées |
| 010 | Persistance temporaire et limites d'effacement | VALIDÉ avec réserve ; stockage non implémenté |

Décisions techniques complémentaires :

- [ADR-011 — Livraison par preuves, modèles isolés](ADR-011-proof-gates.md)
- [ADR-012 — Renderer du moniteur, dépendances épinglées](ADR-012-monitor-renderer.md)

Une modification future d'une décision VALIDÉE doit exposer contexte, options,
conséquences, risques et tests, puis obtenir l'arbitrage du propriétaire. Le fait
qu'un prototype compile ne modifie pas une décision de sécurité ou de périmètre.
