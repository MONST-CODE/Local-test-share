# DevLink 0.2.0-preview — partage temporaire d'une application web locale

**Préversion de développement.** Testée sur une seule machine ; pas encore
qualifiée entre deux ordinateurs d'un même réseau. Ne pas l'exposer à un réseau
non fiable.

- **Lancer** : `bin/devlink-desktop.exe` (poste hôte et invité, Windows 10/11 x64)
  ou `bin/devlink-monitor.exe` (moniteur local en ligne de commande). Les DLL
  nécessaires (SDL3, OpenSSL 3, WebView2Loader, runtime UCRT64) sont dans `bin/`,
  leurs licences dans `licenses/`. L'hôte a besoin du runtime Microsoft Edge
  WebView2 (présent par défaut sur Windows 11). L'archive Linux contient le
  moniteur et le poste desktop, sans partage navigateur (Windows seulement).
- **Hôte** : repère les serveurs locaux, partage l'application choisie dans un
  navigateur isolé qui tourne sur votre machine, approuve ou révoque les invités,
  choisit le mode (A test indépendant, B collaboration, C observation), discute
  et reçoit les signalements d'anomalie.
- **Invité** : rejoint par annonce sur le réseau local ou par adresse et code
  d'invitation, voit les images de la page et la pilote selon les droits que l'hôte
  accorde, discute et signale des anomalies.
- **Sécurité** : l'invité ne reçoit ni le code, ni les fichiers, ni le `.env`, ni
  un accès shell, bureau, base de données ou autre port local. Contrôle et images
  passent par TLS 1.3 ; aucun repli en clair. Rien n'est conservé après la session.
- **Désinstallation** : fermer l'application et supprimer le dossier d'extraction.
  Aucune clé de registre, aucun service en arrière-plan.

## Documentation incluse

- `docs/QUICKSTART.md` : portée et démarrage
- `docs/USER_GUIDE.md` (archive Windows) : partager une application, messagerie, signalements
- `docs/SECURITY.md` : garanties et risques résiduels
- `docs/BUILD.md` : compilation native
- `docs/DEPENDENCIES.md` (et `docs/runtime-dependencies.txt` sous Windows) : dépendances et provenance
- `docs/reports/` : dépendances de build (et, sous Windows, UCRT64 et validation)
- `licenses/` : licences SDL3, Dear ImGui, notices tierces et paquets d'exécution
