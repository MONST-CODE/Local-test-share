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

## 3. Problèmes de partage (navigateur hôte, images, entrées)

### Symptôme : l'interface affiche « moteur navigateur indisponible ».
**Cause :** le runtime Microsoft Edge WebView2 est absent sur l'hôte (il est présent
par défaut sur Windows 11). Le partage réel l'exige ; DevLink refuse alors de
démarrer, sans repli synthétique silencieux.
- *Solution* : installer « Microsoft Edge WebView2 Runtime » (Evergreen), puis
  relancer DevLink. Le partage navigateur n'existe pas encore sous Linux (ADR-018).

### Symptôme : « TLS 1.3 indisponible », le partage refuse de démarrer.
**Cause :** les DLL OpenSSL 3 (`libssl-3-x64.dll`, `libcrypto-3-x64.dll`) livrées à
côté de `devlink-desktop.exe` manquent. Aucun repli en clair n'est proposé.
- *Solution* : réinstaller le paquet complet (dossier `bin` intact).

### Symptôme : l'invité voit une page blanche ou incomplète.
**Cause :** une ressource de la page vient d'une origine non autorisée (API sur un
autre port, CDN). Le navigateur de l'hôte ne sort que vers les origines choisies.
- *Solution* : ajouter l'origine manquante dans « Origines autorisées » (Configuration),
  séparées par des virgules. Les compteurs « bloqués » de l'écran Session active le
  confirment.

### Symptôme : l'invité est admis mais n'affiche aucune image (« hôte injoignable sur le port média »).
**Cause :** le pare-feu Windows de l'hôte bloque le port média, ou le réseau est
classé « public ».
- *Solution* : autoriser `devlink-desktop.exe` sur les **réseaux privés** et classer
  le réseau local comme privé.

### Symptôme : un signalement envoyé n'apparaît pas chez l'hôte.
- Seul l'**accusé de réception** affiché sous le formulaire prouve la réception.
  Sans accusé : vérifier que la session est bien admise (pastille « SESSION NON
  ADMISE ») et que la description n'est pas vide. Un texte trop fréquent est
  limité par les quotas de messagerie de l'hôte.

## 4. Diagnostics et Journaux d'Erreurs

- Dans l'application, l'onglet **Diagnostics système** fournit un affichage en temps réel des erreurs d'entrées/sorties natives, des compteurs de sécurité et des avertissements système.
- Lors de l'exécution en ligne de commande ou via les scripts de test, les journaux détaillés de chaque opération sont archivés dans le dossier `out\windows\logs\`.

---

## 5. Problèmes de build (`build.bat`)

### Symptôme : « un autre build détient out\windows\.build-lock ».
**Cause :** un build est en cours, ou un build précédent a été interrompu (Ctrl+C).
- *Solution* : vérifier qu'aucun `build.bat`, `clang++.exe` ou test DevLink ne
  tourne (Gestionnaire des tâches), puis supprimer le verrou avec la commande
  affichée : `rmdir "…\out\windows\.build-lock"`.

### Symptôme : caractères étranges (`←[38;2;…m`) au lieu des couleurs.
**Cause :** console ancienne sans séquences VT (Windows antérieur à 10).
- *Solution* : `build.bat <opération> --plain`, ou définir `NO_COLOR=1`. La sortie
  reste complète, sans couleurs ni réécriture de ligne.

### Symptôme : une étape échoue, l'erreur affichée ne suffit pas.
- La sortie de la commande fautive est imprimée sous l'étape `×`, suivie du chemin
  de sa capture complète (`out\windows\logs\…command-N.txt`). Pour tout voir en
  direct : relancer avec `--verbose`.
