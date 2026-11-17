# Notes de Version — DevLink 1.0 (Windows x64)

**Date de publication : 17 novembre 2026**  
**Version : 1.0.0**  
**Plateforme cible : Windows 10 / Windows 11 x64**  
**Architecture : C++20 / MSYS2 UCRT64 / SDL3 / Dear ImGui**

---

## Vue d'Ensemble de la Version

La version 1.0.0 de **DevLink** marque l'aboutissement de la transition depuis le prototype initial PLPT vers un produit logiciel desktop natif complet, sécurisé, testé et prêt pour le partage applicatif sur réseau local.

DevLink permet à tout développeur d'ouvrir instantanément ses serveurs locaux (Next.js, Vite, Flask, API conteneurisées, etc.) au test collaboratif sans avoir à déployer son code, sans transmettre son dépôt Git, sans exposer ses fichiers `.env` et sans exiger des testeurs l'installation d'outils de développement.

---

## Nouveautés Majeures

1. **Application Desktop Native Unifiée** :
   - Interface graphique accélérée matériellement avec thème sombre soigné.
   - Séparation claire entre **Espace Hôte** et **Espace Invité** dans un même exécutable portable.
   - Prise en charge native du DPI élevé et du redimensionnement fluide.

2. **Moteur d'Isolation de Navigation** :
   - Rendu exécuté côté hôte avec profils indépendants par invité.
   - Isolation stricte des cookies, sessions et stockage local.
   - Allowlist de sécurité empêchant toute tentative d'accès au système de fichiers (`file://`) ou aux ressources non partagées.

3. **Plan de Contrôle et Réseau LAN Résilient** :
   - Découverte automatique des sessions sur le réseau local via multicast UDP.
   - Codes d'invitation haute entropie avec vérification en temps constant.
   - Validation manuelle des participants à l'entrée et capacité de révocation en un clic.

4. **Plan de Données Proxy Sécurisé** :
   - Accès aux services protégés par un jeton de capacité secret à 32 caractères.
   - Prise en charge complète de HTTP/1.1 et des WebSockets RFC 6455 (HMR, notifications en direct, flux bidirectionnels).

5. **Hub Collaboratif Intégré** :
   - Messagerie de session sécurisée sans service cloud externe.
   - Remontée d'anomalies (Feedback) contextuelle avec niveau de gravité.

---

## Vérification et Qualification

- **Compilateur** : Clang++ 22.1.8 UCRT64 (GNU ABI, x86_64-w64-windows-gnu).
- **Couverture de tests** :
  - 31 cas de tests unitaires : **13 835 assertions réussies, 0 échec**.
  - 7 cas de tests plateforme réseau et sockets : **221 assertions réussies, 0 échec**.
  - Smoke tests graphiques SDL3 / ImGui validés.
  - Fermeture d'import PE vérifiée sans dépendance MSVCRT legacy.
- **Packaging** : Archive portable autonome générée sous `out\windows\packages\`.

---

## Instructions de Lancement

1. Extraire l'archive `PLPT-0.1.0-preview-windows-ucrt64-x64.zip` dans un répertoire utilisateur.
2. Lancer `bin\plpt-desktop.exe`.
3. Sélectionner un service dans le Centre de Découverte, configurer le partage et inviter vos collaborateurs !
