# DevLink — Plateforme Locale de Partage et de Test d'Applications

**C++20 · Clang++ · Windows x64 · SDL3 + Dear ImGui · Architecture Native**

DevLink est une plateforme desktop native de partage temporaire, sécurisé et collaboratif d'applications locales. Elle permet à un développeur de partager une application exécutée sur son propre ordinateur avec plusieurs autres personnes sur le réseau local, sans déployer son application sur un hébergeur, sans transmettre son dépôt Git, sans exposer son système de fichiers ni dépendre d'un tiers de tunnel externe.

---

## Fonctionnalités Principales

- **Découverte Native de Services** :
  - Détection automatique des listeners TCP (IPv4 et IPv6) et des endpoints UDP liés.
  - Association des ports aux PIDs et reconstruction de l'arbre de processus (PPID).
  - Déduction des terminaux d'origine (PowerShell, CMD, Windows Terminal, IDE) avec indice de confiance explicite.
  - Aucune exposition automatique de service : sélection et autorisation strictement explicites par l'hôte.

- **Plan de Contrôle Réseau Sécurisé** :
  - Allocation dynamique d'endpoints réseau (`bind(port=0)` sous le compte utilisateur courant).
  - Annonce et écoute de découverte LAN (Multicast UDP `239.255.77.88` / port 47788).
  - Codes d'invitation cryptographiques aléatoires (haute entropie, format base32 Crockford, temps constant).
  - Authentification mutuelle et file d'attente d'approbation manuelle pour chaque invité.
  - Capacité d'accueil paramétrable (scénario de référence à 5 invités indépendants).

- **Plan de Données et Proxy d'Application** :
  - Pont TCP sécurisé avec contrôle d'accès strict par jeton de capacité (32 caractères hexadécimaux).
  - Transmission transparente des protocoles HTTP/1.1, SSE et WebSockets (RFC 6455).
  - Sondage actif de l'amont local et isolation des flux.

- **Rendu Distant & Isolation des Invités** :
  - Moteur d'isolation de navigation (`IRemoteBrowserBackend` et `ManagedBrowserBackend`).
  - Profils de navigateur indépendants par testeur (cookies, cache et stockage local séparés).
  - Allowlist stricte d'origines (interdiction absolue de `file://`, scripts système et protocoles externes).
  - Nettoyage automatique et purge immédiate des répertoires temporaires lors de la fermeture de session.

- **Hub Collaboratif Intégré** :
  - Messagerie instantanée de session (canal général et messages privés chiffrés sur le canal de contrôle).
  - Statut de présence et mesure de latence réseau réelle en millisecondes.
  - Système de signalement d'anomalies (Feedback) avec gravité (Informatif, Mineur, Majeur, Bloquant) et route concernée.

- **Interface Graphique Native** :
  - Développée avec SDL3 et Dear ImGui, rendu matériel accéléré.
  - Espace Hôte : Tableau de bord, Centre de Découverte, Configuration de Partage, Supervision de Session, Chat, Feedback, Diagnostics.
  - Espace Invité : Découverte automatique des sessions, Connexion, Application Partagée, Chat et Formulaire de Signalement.

---

## Règles de Sécurité Non Négociables

1. **Aucun shell ou terminal invité** sur l'ordinateur hôte.
2. **Aucun accès au bureau** ou partage d'écran généraliste.
3. **Aucun accès au disque dur** ni transmission du code source / fichiers `.env`.
4. **Aucun accès direct** aux bases de données locales (PostgreSQL, Docker Engine, Supabase Studio non exposés).
5. **Moindre privilège** : fonctionne exclusivement sous le compte utilisateur sans élévation administrateur.
6. **Éphémérité** : révocation immédiate et nettoyage des ressources à la fermeture.

---

## Compilation et Qualification (Windows x64)

Prérequis : **MSYS2 UCRT64** avec Clang++ (`C:\msys64\ucrt64\bin\clang++.exe`), SDL3 3.4.20 et binutils natifs.

```bat
:: Vérification de l'environnement
.\build.bat doctor --verbose

:: Téléchargement et vérification des dépendances épinglées (ImGui / SDL3)
.\build.bat deps --fetch

:: Compilation du projet en mode Debug
.\build.bat debug

:: Exécution de la suite complète de tests (31 cas unitaires, 7 cas système, UI smoke tests)
.\build.bat test

:: Lancement de l'application desktop DevLink
.\out\windows\debug\bin\plpt-desktop.exe

:: Compilation et packaging de la version de production
.\build.bat release
.\build.bat package
```

Le livrable final est généré sous forme d'archive autonome dans `out\windows\packages\`.

---

## Structure du Dépôt

- `app/` :
  - `desktop_ui/` : Interface graphique native DevLink (SDL3 / ImGui).
  - `host_shell/` : Moniteur CLI d'inspection locale.
  - `demo_service/` : Service HTTP/WebSocket local pour la qualification de bout en bout.
- `engine/` :
  - `core/` : Types forts, Result, digests SHA-256, secrets, horloge.
  - `discovery/` : Arbre de processus, observateur de sockets, classificateur de terminaux.
  - `service_registry/` : Registre des services et modélisation des états de partage.
  - `port_allocator/` : Allocation dynamique de ports OS sans TOCTOU.
  - `net/` : Wire protocol, discovery multicast, HostServer, GuestClient, ServiceProxy.
  - `browser/` : Moteur d'isolation de profil, allowlist et capture (`IRemoteBrowserBackend`).
  - `messaging/` : Modèles de chat, quotas et remontée d'anomalies.
  - `platform/` : Adaptateurs système Windows et Linux.
- `docs/` : Documentation d'architecture, réseau, protocole, sécurité et guides d'utilisation.
- `tests/` : Suites de tests unitaires, d'intégration, de conformité plateforme et de robustesse.
