# Guide d'Utilisation de DevLink

Ce guide détaille les parcours d'utilisation de **DevLink** pour l'hôte (développeur) et pour les invités (testeurs / collaborateurs).

---

## 1. Parcours Développeur (Espace Hôte)

### Étape 1 : Démarrer vos services de développement locaux
Lancez vos applications comme vous le faites habituellement dans votre terminal ou IDE :
```bash
# Exemple Next.js
npm run dev

# Ou exemple conteneurs Docker
docker compose up -d

# Ou exemple API Python Flask
flask run --port 5000
```

### Étape 2 : Ouvrir DevLink et Découvrir les Services
1. Lancez `plpt-desktop.exe`.
2. Dans la barre latérale gauche, sélectionnez **Centre de découverte**.
3. DevLink affiche instantanément l'inventaire des sockets en écoute (TCP/UDP), les PIDs associés et l'origine probable (PowerShell, CMD, VS Code, Terminal).
4. Cliquez sur le service Web que vous souhaitez partager (par exemple votre port Next.js `3000`), puis cliquez sur **Choisir** ou **Sélectionner comme point d'entrée**.

### Étape 3 : Configurer et Démarrer la Session
1. Rendez-vous dans l'onglet **Configurer le partage**.
2. Renseignez :
   - Le **Nom de la session** (ex. `Test Sprint 4`).
   - Votre **Nom d'hôte** (ex. `Alex`).
   - Le **Nombre maximum d'invités** (défaut : 5).
   - Le **Mode de participation** :
     - *Test indépendant* : chaque testeur dispose de ses propres cookies et session applicative.
     - *Collaboration* : vue partagée avec contrôle exclusif.
     - *Observation seule* : flux vidéo passif sans interaction clavier/souris.
   - Cochez **Exiger une validation manuelle** pour approuver chaque invité avant admission.
3. Cliquez sur **Lancer la Session DevLink**.

### Étape 4 : Inviter des Testeurs et Modérer la Session
1. L'écran bascule automatiquement sur **Session active**.
2. Communiquez à vos testeurs le **Code d'invitation** affiché en haut (ex. `K7QF-2MXA-9TRD-4BCS`).
3. Lorsque les testeurs saisissent le code, une demande apparaît sous **Demandes d'Adhésion en Attente** :
   - Cliquez sur **Autoriser** pour admettre l'invité.
   - Cliquez sur **Refuser** pour rejeter la demande.
4. Dans le tableau des participants, vous pouvez surveiller l'état et la latence en temps réel de chaque invité, ou cliquer sur **Révoquer** pour déconnecter un participant instantanément.

### Étape 5 : Communiquer et Consulter les Signalements
- **Messagerie** : Cliquez sur l'onglet **Messagerie de session** pour discuter avec vos testeurs ou envoyer des directives de test.
- **Retours & Feedback** : Cliquez sur **Retours & Feedback** pour consulter la liste des bugs signalés par les testeurs (gravité, page concernée, description détaillée).

### Étape 6 : Clôture de la Session
Cliquez sur **Fermer la Session Définitivement** :
- Tous les flux et sockets sont instantanément coupés.
- Les jetons d'accès et profils temporaires sont purgés.
- Les invités sont déconnectés sans conserver aucun accès.

---

## 2. Parcours Testeur (Espace Invité)

### Étape 1 : Rejoindre une Session
1. Lancez `plpt-desktop.exe` sur votre ordinateur.
2. Cliquez en haut à droite sur **[ Espace Invité ]**.
3. Dans l'onglet **Rejoindre une session** :
   - Si la découverte réseau automatique est active, la session de votre collègue apparaît dans la liste **Sessions Détectées sur le Réseau**. Cliquez sur **Sélectionner**.
   - Sinon, saisissez manuellement l'adresse IP de l'hôte et le port de contrôle.
4. Saisissez votre **Nom de testeur** et le **Code d'invitation** fourni par l'hôte.
5. Cliquez sur **Rejoindre la Session**.

### Étape 2 : Attente et Validation
- Si l'hôte a configuré la validation manuelle, le statut indique *En attente de l'autorisation de l'hôte*.
- Dès que l'hôte valide, votre statut passe à *Connecté*.

### Étape 3 : Tester l'Application
1. Rendez-vous dans l'onglet **Application partagée**.
2. Cliquez sur **Lancer le Navigateur de Test Isolé** ou copiez l'URL sécurisée tokenisée pour tester l'application.
3. Vous disposez d'un profil de test indépendant : vos cookies, logins de test et paniers d'achat n'écrasent pas ceux des autres testeurs.

### Étape 4 : Échanger et Signaler des Anomalies
- Utilisez l'onglet **Messagerie de session** pour échanger des messages avec l'hôte et l'équipe.
- En cas de problème ou de bug, rendez-vous dans **Signaler une anomalie** :
  - Choisissez la gravité (Informatif, Mineur, Majeur, Bloquant).
  - Indiquez la page testée et décrivez le problème.
  - Cliquez sur **Transmettre le Signalement** pour notifier immédiatement l'hôte.

### Étape 5 : Quitter la Session
Cliquez sur **Se déconnecter** pour quitter proprement la session.
