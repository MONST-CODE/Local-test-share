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
- [ADR-013 — Rôle du proxy et frontière de confiance avec le rendu distant](ADR-013-proxy-vs-rendu-distant.md)
- [ADR-014 — Identité du participant, reprise de place et exposition réseau](ADR-014-identite-reprise-exposition.md)
- [ADR-015 — Identité publique DevLink, version unique et identifiants internes stables](ADR-015-identite-publique-et-version-unique.md)
- [ADR-016 — Transport des sessions réelles : interface versionnée, TLS 1.3 PSK authentifié, disponibilité honnête](ADR-016-transport-tls13-psk.md)
- [ADR-017 — Authentification par défi-réponse et basculement du plan de contrôle sur TLS 1.3](ADR-017-defi-reponse-et-basculement-tls13.md)

Décidé par ADR-016 : le transport sécurisé des sessions réelles est TLS 1.3 à
PSK externe (pile OpenSSL chargée à l'exécution sur Linux ; Windows à câbler —
IMPLEMENTED_BUT_NOT_VERIFIED, voir l'ADR).

Décidé et **appliqué** par ADR-017 : le plan de contrôle est passé sur ce
transport (protocole 1.3). Le secret ne transite plus : l'hôte envoie un nonce,
l'invité prouve la connaissance du code d'invitation ou du secret de reprise par
HMAC-SHA256 (RFC 2104, `engine/core/mac.*`), puis les deux côtés dérivent le
même PSK et basculent avant toute trame utile. Un client qui transmettrait
encore un secret en clair est refusé. Sans runtime TLS, un partage réel est
**refusé** (`tls_required`, défaut true) ; le clair n'existe que s'il est assumé
des deux côtés, et il est alors étiqueté partout. Le canal historique XOR/HMAC
(`secure_channel`) est **supprimé** du dépôt : plus aucune crypto maison n'est
liée, en production comme dans les tests.
À instruire (aucune décision prise à ce jour) : Browser Worker / capture de
surface (prototype WebView2 et sa porte de validation avant toute interface
définitive).

Une modification future d'une décision VALIDÉE doit exposer contexte, options,
conséquences, risques et tests, puis obtenir l'arbitrage du propriétaire. Le fait
qu'un prototype compile ne modifie pas une décision de sécurité ou de périmètre.
