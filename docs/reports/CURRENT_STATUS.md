# État courant du moteur — chiffres et statuts datés

Date : **2026-11-14**. Branche de travail : `arena/1a188065-local-test-share`,
base `main` `7374734` (fusion de la PR #4). Ce document remplace les chiffres des
rapports antérieurs **sans les réécrire** : les rapports datés du 2026-11-13 restent
des preuves historiques de leur propre build.

## Ce qui a été mesuré, et avec quoi

| Profil | Commande réelle | Résultat |
| --- | --- | --- |
| Unitaire, `-O0 -Wall -Wextra -Werror` | `plpt-unit-tests` | **33 cas / 20 882 vérifications / 0 échec** |
| Intégration LAN, `-O0` | `plpt-platform-tests` | **10 cas / 513 vérifications / 0 échec** |
| ASan + UBSan, `-O1`, fuites activées | les deux binaires | mêmes totaux, **0 échec**, aucun rapport de sanitizer |
| `_GLIBCXX_ASSERTIONS`, `-O0` (profil de débogage Windows) | les deux binaires | mêmes totaux, **0 échec**, aucune assertion libstdc++ |
| Service de démonstration, exécution réelle | 85 requêtes HTTP dont 30 parallèles | 9 threads stables (1 + 8 ouvriers), 40/40 réponses `200` après saturation, `426` sans upgrade WebSocket, arrêt en 100 ms après `SIGTERM` |
| Énumération des interfaces, exécution réelle | `resolve_bind_address` | `lo` 127.0.0.1 / ::1 et `eth0` 169.254.0.21 / fe80::fc:ff:fe00:5 %2 ; eth0 retenue, repli lien local signalé |

**Chaîne de compilation de ces mesures :** `g++ 12.2.0` sur Debian 12, en tant
qu'outil de vérification intermédiaire uniquement (aucun `clang` n'est installé dans
cet environnement et `PLPT_CXX` n'a jamais été pointé sur `g++`). La **qualification**
appartient à la CI : `clang++ 18` natif Linux et `clang++` MSYS2 UCRT64 natif
Windows, via `build.sh` et `build.bat`. Un lot n'est qualifié qu'après un run CI vert
sur les deux jobs ; les totaux ci-dessus sont donc des résultats d'exécution réelle,
pas une qualification de livraison.

Cas d'intégration exécutés sur TCP/UDP **réels en boucle locale** : plan de contrôle
hôtes + 3 invités (authentification, approbation, messagerie générale et privée,
battements et latence mesurée, capacité atteinte, révocation, fermeture en
`Closed`), plan de données (autorisation par jeton, retrait de la capacité, cookie de
capacité, 403/502, amont injoignable), reprise de place par secret, découverte
(annonce et écoute multicast en boucle locale). Le mode `--ipv6` sort en 77 (SKIP)
faute d'IPv6 dans l'environnement : un SKIP n'est jamais compté comme un PASS.

## Statuts par domaine

| Domaine | Statut | Preuve / réserve |
| --- | --- | --- |
| Détection native (processus, ports, arbre, classification) | **IMPLÉMENTÉ, TESTÉ** | cas unitaires et plateforme ; Linux `proc`, adaptateurs Windows |
| Registre de services et sélection | **IMPLÉMENTÉ, TESTÉ** ; non relié au partage | observation seulement, jamais une autorisation |
| Allocation de port (port 0, socket conservée, fds rendus) | **IMPLÉMENTÉ, TESTÉ** | `ipv4_allocation`, `repeated_allocation_releases_fds` |
| Énumération d'interfaces et adresse de liaison | **IMPLÉMENTÉ, TESTÉ** | ADR-014 ; instantané au démarrage, pas de basculement en session |
| Plan de contrôle hôte/invité (protocole 1.2) | **IMPLÉMENTÉ, INTÉGRÉ, TESTÉ** en boucle locale | TCP en clair : non qualifié pour une session réelle |
| Identité et reprise de place | **IMPLÉMENTÉ, TESTÉ** | secret aléatoire rotaté ; ADR-014 |
| Messagerie (général, DM, quotas, rédaction) | **IMPLÉMENTÉ, TESTÉ** | présence, réactions, mentions, modération : à faire |
| Proxy de service (capacité retirée, cookie de capacité) | **IMPLÉMENTÉ, TESTÉ** en boucle locale | HTTP clair, pas de HTTP/2 ni de demi-fermeture ; ADR-013 |
| Découverte LAN (multicast, broadcast, saisie manuelle) | **IMPLÉMENTÉ, TESTÉ** en boucle locale | multicast inter-machines **NON QUALIFIÉ** ; mDNS/DNS-SD non évalué |
| Browser Worker, capture, encodage, décodage, rendu distant | **ABSENT** | `NullBrowserBackend` échoue explicitement ; aucun faux rendu |
| Interface SDL3/ImGui branchée sur le réseau | **NON IMPLÉMENTÉ** | le moniteur local fonctionne ; aucune session ouverte depuis l'UI |
| Orchestration (détection → registre → allocation → serveur → proxy → worker) | **NON IMPLÉMENTÉ** | aucun enchaînement câblé |
| Nettoyage et profils navigateur temporaires | **PARTIEL** | fermeture `Closing→Cleanup→Closed` prouvée ; stockage temporaire indisponible, explicitement |
| Transport sécurisé (TLS/QUIC) | **NON IMPLÉMENTÉ** | ADR de transport à instruire ; aucun repli silencieux ajouté |
| Charge 5 invités / 30 FPS / 720p / p95 < 150 ms | **NON QUALIFIÉ** | aucune mesure revendiquée |
| LAN physique deux machines | **NON QUALIFIÉ** | une seule machine disponible : procédure propriétaire à écrire, critères à marquer NON QUALIFIÉS |

## Défauts corrigés dans cet incrément

1. Fuite du jeton de capacité vers le service local (ADR-013).
2. Sous-ressources d'une page autorisée refusées en `403` (ADR-013).
3. Réattribution d'une place sur simple homonymie (ADR-014).
4. Écoute sur toutes les interfaces par défaut, sans énumération réelle possible (ADR-014).
5. Threads du service de démonstration jamais joints, `detach()` à la sortie, et
   saturation définitive après 8 connexions (pool fixe, ouvriers joints).
6. Purge des demandes en attente par couple (nom, adresse), ambiguë.

## Reste ouvert, par ordre de risque

1. **Browser Worker / capture / encodage / décodage** : le différenciateur fondamental
   du produit est absent. Prototype WebView2 sous clang++ UCRT64 et porte de
   validation (DPI 100/150/200 %, fenêtre minimisée ou occluse, entrées, fermeture,
   suppression des données temporaires) avant toute interface définitive.
2. **Transport sécurisé** : comparaison de piles QUIC/TLS maintenues compatibles avec
   les deux chaînes imposées ; aucune réimplémentation de TLS/QUIC ; aucune session
   réelle en TCP clair.
3. **Interface branchée** sur `host_server`/`guest_client` via instantanés immuables
   et commandes sur files bornées.
4. **Orchestration** et cycle de vie complet (§9 du maître), récupération après crash.
5. **Qualification LAN physique** et charge mesurée.
