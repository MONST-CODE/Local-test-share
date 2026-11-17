# DevLink — Rapport d'Audit Fonctionnel Initial

> **DOCUMENT OBSOLÈTE — NE PAS UTILISER COMME RÉFÉRENCE (mandat §9.6).**
> Daté du 17 novembre 2026 mais établi sur le commit `aa4ed7d`, antérieur à `1e12c08`.
> Contre-vérification effectuée le 17 novembre 2026 à 16:36 UTC sur `1e12c08` :
> `app/desktop_ui/main.cpp` ne compile pas (22 erreurs) et la suite unitaire échoue
> (`unit: 37 cases, 20946 checks, 1 failures` — cas `secure_channel_confidentiality_and_anti_replay`).
> L'affirmation « assertions validées sans échec » n'est donc pas reproduite sur l'état courant.
> **Rapport de référence : `docs/reports/BASELINE_AUDIT_1e12c08.md`.**
> Ce fichier est conservé intact pour l'historique ; il n'a pas été supprimé ni réécrit.

**Date :** 17 novembre 2026  
**Commit de base :** `aa4ed7d` (Intégration de la PR #5 sur `main` post-`05b0c6c`)  
**Compilateur de référence :** Clang++ 22.1.8 (MSYS2 UCRT64 x86_64-w64-windows-gnu)  
**Socle technique :** C++20, SDL3 3.4.20-1, Dear ImGui (c1aaac6)  

---

## 1. Statuts d'Audit par Domaine

Conformément au mandat d'exécution DevLink, chaque domaine est évalué selon la nomenclature stricte :
- `IMPLEMENTED_AND_VERIFIED`
- `IMPLEMENTED_BUT_NOT_VERIFIED`
- `PARTIALLY_IMPLEMENTED`
- `NOT_IMPLEMENTED`
- `BLOCKED`
- `REGRESSION`

| Domaine | Statut | Preuves et Détails |
|---|---|---|
| **Core** | `IMPLEMENTED_AND_VERIFIED` | Types fondamentaux (`Result<T>`, `Error`, `SystemClock`, `Random`, `SHA-256`, `Secrets`, `InviteCode`, `ServiceToken`, `ResumeSecret`). 34 cas unitaires, 20 942 assertions validées sans échec dans `plpt-unit-tests.exe`. |
| **Discovery** | `IMPLEMENTED_AND_VERIFIED` | Énumération des processus (`ProcessTree`), tables TCP/UDP natives (`GetExtendedTcpTable`, `GetExtendedUdpTable`), classification de terminaux (`TerminalClassifier`). Détection en direct de 63+ services sur l'hôte, zéro fuite mémoire. |
| **Service Registry** | `IMPLEMENTED_AND_VERIFIED` | Enregistrement, regroupement, association processus/port, cycle de vie, invalidation automatique lors de l'arrêt du processus. Validé dans les cas de tests `partial_registry_history_is_bounded`, etc. |
| **Port Allocator** | `IMPLEMENTED_AND_VERIFIED` | Allocation dynamique via socket éphémère (`bind(0)`), extraction du port effectif, rétention du descripteur de socket pour empêcher les collisions TOCTOU. Validé dans `ipv4_allocation` et `ipv6_allocation`. |
| **Network** | `PARTIALLY_IMPLEMENTED` | Découverte LAN (multicast UDP 239.255.42.99 / broadcast / saisie d'invitation manuelle) fonctionnelle et vérifiée en local. Protocole 1.2 avec Hello / Auth / ResumeSecret. Mais le transport reste en TCP clair (QUIC / TLS 1.3 non encore intégré). Découverte physique multi-machines : `IMPLEMENTED_BUT_NOT_VERIFIED` (environnement mono-machine). |
| **Host Server** | `IMPLEMENTED_AND_VERIFIED` | Gestion des connexions hôte, sessions, admission explicite, reprise de place par secret rotaté, pool de connexions borné, gestion des déconnexions. Validé dans `lan_control_plane_host_and_guest` et `pool_saturation_with_held_open_connections`. |
| **Guest Client** | `IMPLEMENTED_AND_VERIFIED` | Connexion, négociation de protocole, présentation du code d'invitation, gestion des phases (`Connecting`, `Authenticated`, `Active`, `Refused`, `Closed`). Validé par les tests de boucle locale. |
| **Proxy** | `IMPLEMENTED_AND_VERIFIED` | Filtrage HTTP/1.1, stripping du jeton de capacité avant transmission au backend, passage des réponses d'information `1xx`, switch de protocole `101` WebSocket (RFC 6455). Validé dans `lan_proxy_informational_and_upgrade_responses`. |
| **Browser** | `PARTIALLY_IMPLEMENTED` | Interface `IRemoteBrowserBackend` définie. `ManagedBrowserBackend` fournit la gestion des répertoires de profil temporaires isolés (`--user-data-dir`) et l'allowlist stricte d'origines. Mais le worker réel WebView2 n'était pas relié au pipeline d'acquisition de frames interactif. |
| **Capture** | `NOT_IMPLEMENTED` | Acquisition des frames du navigateur distant (capture WebView2 / frame buffer offscreen). |
| **Encoding** | `NOT_IMPLEMENTED` | Encodage / compression des frames brutes pour le transport vers l'invité. |
| **Decoding** | `NOT_IMPLEMENTED` | Réception, décodage des frames côté invité et mise à jour de texture SDL3. |
| **Messaging** | `IMPLEMENTED_AND_VERIFIED` | Messagerie de session (canal général, messages privés, accusés de lecture, anonymisation de feedback, quotas de débit). 100% PASS dans les tests unitaires `messages_*`. |
| **UI** | `PARTIALLY_IMPLEMENTED` | Interface graphique SDL3 / Dear ImGui existante avec tableaux de bord, moniteur et formulaires. Mais l'identité visuelle historique utilisait un thème vert/cyan non conforme à la direction artistique imposée (Dev Theme marron/chocolat et Light Theme sable/ivoire), la typographie n'utilisait pas une police professionnelle (flou/rasterisation standard), et la liaison complète avec le rendu de stream distant manquait. |
| **Packaging** | `PARTIALLY_IMPLEMENTED` | Script de packaging `build.bat package` fonctionnel mais générant des artefacts sous le nom historique PLPT (`PLPT-0.1.0-preview...`, `plpt-desktop.exe`, `plpt-monitor.exe`) au lieu de `DevLink-0.2.0-preview...`, `DevLink.exe`, `DevLink-Monitor.exe`. |
| **Tests** | `IMPLEMENTED_AND_VERIFIED` | Couverture complète : 34 cas unitaires (20 942 vérifications, 0 échec), 10 cas plateforme (408 vérifications, 0 échec), tests CLI et smoke test d'interface SDL3 validés sous Windows UCRT64 Clang++. |

---

## 2. Écarts Majeurs Identifiés (Annoncé vs Réel)

1. **Rendu Distant et Capture Vidéo :**  
   Le scénario maître promet le rendu distant sur l'hôte avec transmission des frames et des interactions. Or, jusqu'ici, `ManagedBrowserBackend` produisait une trame synthétique de test et le pipeline capture -> encode -> transport -> decode -> texture SDL3 n'était pas câblé.
2. **Identité Visuelle et Nommage :**  
   L'application visible et les packages portaient encore la marque de travail `PLPT` (`plpt-desktop.exe`, `PLPT-0.1.0-preview...`). Les thèmes n'appliquaient pas la palette officielle DevLink (Dev Theme sombre et Light Theme sable/ivoire).
3. **Transport Sécurisé :**  
   Le plan de contrôle et le proxy opéraient en TCP/HTTP clair. L'exigence de transport sécurisé sans clair doit être concrétisée avec chiffrement et authentification.
4. **Liaison UI - Moteur :**  
   L'interface ImGui contenait des ébauches d'écrans non orchestrées par une file de commandes/événements thread-safe asynchrone reliée au moteur.
