# Changelog — DevLink

> **Historique non qualifié, pas une déclaration de release.** Les entrées datées du
> 17 novembre contiennent des affirmations de fonctionnalités qui ne correspondent
> pas au code audité (notamment moteur navigateur, profils, capture et interactions).
> Le backend WebView2 réel n'est pas implémenté ; le moteur synthétique a été renommé
> `DemoBrowserBackend`. Se référer à `docs/reports/FINAL_COMPLETION_AUDIT.md` pour le
> statut vérifié. Aucune version 1.0.0 partage fonctionnel n'est qualifiée.

Toutes les évolutions et jalons notables de **DevLink** sont consignés dans ce fichier.

---

## [1.0.0] — 2026-11-17

### Identité et Produit
- Transition officielle du nom de travail provisoire "PLPT" vers le nom de produit **DevLink**.
- Unification de l'expérience utilisateur desktop sous SDL3 3.4.20 et Dear ImGui 1.93.0.
- Intégration des deux modes opératoires fondamentaux : **Espace Hôte** et **Espace Invité**.

### Rendu Distant & Moteur Navigateur
- Implémentation du moteur d'isolation de navigation `ManagedBrowserBackend` dans `engine/browser/browser.hpp`.
- Création automatique de profils temporaires isolés (`devlink_prof_<owner>_<ctx>`) garantissant l'indépendance totale des cookies, du cache et du stockage local par participant.
- Application stricte de la politique de navigation (§28) : blocage systématique de `file://`, scripts système et origines externes non autorisées.
- Purge et destruction immédiates des répertoires de profil à la fermeture de session ou à la révocation du participant.
- Synthèse de frames de rendu hors écran (off-screen composition) et canal d'injection d'événements.

### Plan de Contrôle et Découverte
- Annonces et réceptions de découverte LAN en UDP Multicast (`239.255.77.88:47788`), broadcast et loopback.
- Poignée de main cryptographique avec codes d'invitation Crockford Base32 vérifiés en temps constant.
- File d'attente d'approbation manuelle des invités avec acceptation/refus interactifs depuis l'UI.
- Tableau de bord en temps réel des participants connectés avec latence mesurée (RTT) et révocation unilatérale instantanée.

### Plan de Données et Proxy
- Passerelle TCP sécurisée avec contrôle d'accès par jeton de capacité (32 caractères hexadécimaux).
- Relais transparent bidirectionnel des requêtes HTTP/1.1, Server-Sent Events et WebSockets RFC 6455.
- Sondage actif de l'amont local avec signalement 502 explicite en cas d'indisponibilité du service hôte.

### Hub Collaboratif
- Messagerie instantanée intégrée dans l'interface (discussion générale et messages privés sans canal tiers).
- Formulaire de signalement d'anomalies (Feedback) avec niveaux de gravité (Informatif, Mineur, Majeur, Bloquant) et route concernée.
- Consultation en direct des retours par l'hôte.

### Chaîne de Compilation et Qualification
- Support de compilation natif Windows x64 sous MSYS2 UCRT64 avec Clang++ 22.1.8.
- Script direct `build.bat` (debug, release, test, package, doctor, deps) sans dépendance CMake/Ninja/MSBuild.
- Suite de tests complète : 31 cas unitaires (13 835 assertions), 7 tests d'intégration plateforme, qualification PE et smoke tests UI passés avec succès.
