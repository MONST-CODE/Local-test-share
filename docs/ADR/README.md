# Registre des décisions d'architecture

La source normative des décisions 001–010 reste le [maître §24](../ARCHITECTURE_MASTER.md#24-décisions-darchitecture-adr-initiales).
Les statuts « validé » ci-dessous sont ceux du propriétaire, pas une certification
par les tests de cette branche.

| ADR | Décision | Statut du maître / état d'implémentation |
|---|---|---|
| 001 | C++20, SDL3, Dear ImGui | VALIDÉ ; moniteur natif livré |
| 002 | Windows x64 avant Linux | VALIDÉ ; CI Windows prioritaire, runtime Windows non testé ici |
| 003 | Aucun tunnel tiers | VALIDÉ ; aucun service de ce type ajouté |
| 004 | Rendu exécuté sur l'hôte | VALIDÉ priorité ; implémenté par ADR-018 (WebView2, Windows), qualifié sur une machine |
| 005 | Découverte ports/processus avec confiance | VALIDÉ ; code et tests de socle |
| 006 | Données sur ports OS dynamiques | RECOMMANDÉ ; prototype réservation, pas de transport |
| 007 | QUIC/TLS via pile maintenue | RECOMMANDÉ ; pile/version/licence à choisir et prouver |
| 008 | Messagerie mémoire, hôte endpoint | VALIDÉ ; chat, messages privés et signalements sur le contrôle TLS 1.3 (ADR-017, ADR-019) |
| 009 | Multi-invités | VALIDÉ ; cinq invités en boucle locale (`devlink-share-tests`), pas cinq machines |
| 010 | Persistance temporaire et limites d'effacement | VALIDÉ avec réserve ; stockage non implémenté |

Décisions techniques complémentaires :

- [ADR-011 — Livraison par preuves, modèles isolés](ADR-011-proof-gates.md)
- [ADR-012 — Renderer du moniteur, dépendances épinglées](ADR-012-monitor-renderer.md)
- [ADR-013 — Rôle du proxy et frontière de confiance avec le rendu distant](ADR-013-proxy-vs-rendu-distant.md)
- [ADR-014 — Identité du participant, reprise de place et exposition réseau](ADR-014-identite-reprise-exposition.md)
- [ADR-015 — Identité publique DevLink, version unique et identifiants internes stables](ADR-015-identite-publique-et-version-unique.md)
- [ADR-016 — Transport des sessions réelles : interface versionnée, TLS 1.3 PSK authentifié, disponibilité honnête](ADR-016-transport-tls13-psk.md)
- [ADR-017 — Authentification par défi-réponse et basculement du plan de contrôle sur TLS 1.3](ADR-017-defi-reponse-et-basculement-tls13.md)
- [ADR-018 — Rendu distant réel : Browser Worker WebView2, canal média TLS 1.3 par invité, autorité des entrées côté hôte](ADR-018-rendu-distant-reel-webview2.md)
- [ADR-019 — Signalements d'anomalie et messages privés vers l'hôte (protocole 1.5)](ADR-019-signalements-et-messages-prives-protocole-1-5.md)

Décidé par ADR-016 : le transport sécurisé des sessions réelles est TLS 1.3 à
PSK externe (pile OpenSSL 3 chargée à l'exécution : système sous Linux, DLL UCRT64
épinglées livrées à côté de l'application sous Windows depuis `41bd853`).

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
Décidé et **implémenté** par ADR-018 : Browser Worker WebView2 hors écran (un
profil par contexte), images du compositeur, garde réseau SOCKS5, canal média
TLS 1.3 PSK par invité, autorité des entrées côté hôte (modes A/B/C). Qualifié
en boucle locale sur Windows ; LAN physique encore à exécuter.

Décidé et **implémenté** par ADR-019 (protocole 1.5) : signalement d'anomalie
invité → hôte borné et accusé à l'auteur seul, identifiant public de l'hôte
dans `SessionInfo` pour les messages privés vers l'hôte.

Une modification future d'une décision VALIDÉE doit exposer contexte, options,
conséquences, risques et tests, puis obtenir l'arbitrage du propriétaire. Le fait
qu'un prototype compile ne modifie pas une décision de sécurité ou de périmètre.
