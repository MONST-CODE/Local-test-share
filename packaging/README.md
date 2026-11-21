# DevLink 1.0 — Plateforme Native de Partage et de Test Local

**Partage applicatif distant temporaire, chiffré et collaboratif.**

- **Windows x64** (environnement natif UCRT64) : lancer `bin/devlink-desktop.exe` pour l'interface graphique complète ou `bin/devlink-monitor.exe` pour le moniteur local. Les bibliothèques dynamiques requises (`SDL3.dll`, runtimes C++/UCRT) et leurs licences sont incluses dans `bin/` et `licenses/`.
- **Espace Hôte** : détection de services locaux, réservation de ports dynamiques, proxy sécurisé par jeton de capacité, gestion des approbations d'invités, messagerie temps réel et retours d'anomalies.
- **Espace Invité** : découverte mDNS/multicast sur le réseau local (239.255.77.88), validation d'invitation cryptographique, contextes de navigation isolés et chat collaboratif.
- **Sécurité** : aucun accès shell, aucun accès bureau, aucun transfert de fichiers sources ou variables `.env`, aucun accès direct aux bases de données locales.
- **Désinstallation** : fermer l'application et supprimer le dossier d'extraction. Aucun résidu de base de registre, aucun service persistant en arrière-plan.

## Documentation incluse

- `docs/QUICKSTART.md` : Prise en main rapide
- `docs/BUILD.md` : Guide de compilation native
- `docs/SECURITY.md` : Modèle de sécurité et règles d'isolation
- `docs/NETWORK.md` : Architecture réseau (plan de contrôle et plan de données)
- `licenses/` : Licences SDL3, Dear ImGui et notices tierces.
