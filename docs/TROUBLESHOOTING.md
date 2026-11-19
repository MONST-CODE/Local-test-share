# Guide de Dépannage (Troubleshooting) — DevLink

Ce document recense les problèmes de configuration et de réseau fréquents lors de l'utilisation de **DevLink** et leurs résolutions.

---

## 1. Problèmes de Découverte Réseau Local

### Symptôme : L'invité ne voit pas la session dans la liste des sessions détectées.
**Causes possibles et solutions :**
1. **Isolation des clients Wi-Fi** : De nombreux routeurs d'entreprise ou box Wi-Fi activent l'isolation AP (Access Point Isolation), ce qui bloque les paquets multicast/broadcast entre deux ordinateurs sans fil.
   - *Solution* : Utilisez la **connexion manuelle** dans l'Espace Invité en saisissant directement l'adresse IP de l'hôte (affichée dans le tableau de bord de l'hôte) et le port de contrôle.
2. **Pare-feu Windows bloquant les paquets UDP** :
   - *Solution* : Vérifiez que Windows Defender Firewall autorise `devlink-desktop.exe` sur les réseaux privés.
3. **Multiples interfaces réseau (VPN, Docker, VirtualBox, WSL)** :
   - Si un VPN (ex. WireGuard, OpenVPN, Tailscale) ou un switch virtuel est actif, l'adresse de diffusion peut emprunter une interface virtuelle.
   - *Solution* : Vérifiez dans l'onglet Diagnostics quelle interface est utilisée et privilégiez la connexion par IP directe.

---

## 2. Problèmes de Connexion et d'Invitation

### Symptôme : L'invité reçoit le message "Code d'invitation refusé".
**Causes possibles et solutions :**
1. **Erreur de saisie du code** :
   - Les codes DevLink utilisent l'alphabet Crockford Base32 (chiffres et lettres sans ambiguïté : 1 et I, 0 et O sont automatiquement normalisés).
   - *Solution* : Utilisez le bouton "Copier le code" côté hôte et collez-le côté invité.
2. **Session fermée ou expirée** :
   - Si l'hôte a arrêté la session, le code est immédiatement invalidé. L'hôte doit démarrer une nouvelle session et fournir le nouveau code.

### Symptôme : L'invité reste bloqué en "En attente de l'autorisation de l'hôte".
- C'est le comportement nominal si l'hôte a activé l'option *Exiger une validation manuelle*.
- *Solution* : L'hôte doit se rendre sur son écran **Session active** et cliquer sur **Autoriser** sous la section *Demandes d'adhésion en attente*.

---

## 3. Problèmes Applicatifs et Web

### Symptôme : Le navigateur affiche une erreur "502 Bad Gateway" lors de l'accès au service.
**Cause :** Le service local amont (par ex. Next.js, Flask ou Vite) s'est arrêté ou n'est plus en écoute sur le port configuré.
- *Solution* : Vérifiez dans votre terminal si votre serveur de développement local fonctionne toujours (ex. `npm run dev` ou `docker ps`).

### Symptôme : L'interface affiche « moteur navigateur indisponible ».
**Cause :** Le backend WebView2, la capture réelle et l'injection d'entrées ne sont pas implémentés. La validation d'origine HTTP(S) est une primitive isolée ; elle ne filtre pas encore les navigations d'un navigateur opérationnel.
- *Solution* : Il n'existe pas de configuration utilisateur permettant de débloquer le partage réel dans cette préversion. N'utilisez pas le mode de démonstration synthétique comme substitut ; suivre les étapes de qualification dans `docs/reports/FINAL_COMPLETION_AUDIT.md`.

---

## 4. Diagnostics et Journaux d'Erreurs

- Dans l'application, l'onglet **Diagnostics système** fournit un affichage en temps réel des erreurs d'entrées/sorties natives, des compteurs de sécurité et des avertissements système.
- Lors de l'exécution en ligne de commande ou via les scripts de test, les journaux détaillés de chaque opération sont archivés dans le dossier `out\windows\logs\`.
