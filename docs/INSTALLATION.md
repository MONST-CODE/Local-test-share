# Guide d'Installation de DevLink

Ce document décrit les procédures d'installation, de prérequis et de déploiement de **DevLink** sur les machines Windows de développement et de test.

---

## 1. Prérequis Système

### Ordinateur Hôte (Développeur)
- **Système d'exploitation** : Windows 10 ou Windows 11 (64 bits).
- **Processeur** : Processeur x64 compatible AVX (Intel Core i3/i5/i7/i9 ou AMD Ryzen).
- **Mémoire RAM** : 4 Go minimum (8 Go recommandés pour 5 invités indépendants).
- **Réseau** : Carte réseau Ethernet ou Wi-Fi connectée à un réseau local supportant le trafic multicast / UDP local.
- **Droits utilisateur** : Compte utilisateur standard (aucune élévation administrateur n'est requise pour l'exécution).

### Ordinateur Invité (Testeur)
- **Système d'exploitation** : Windows 10 ou Windows 11 (64 bits).
- **Dépendances tierces** : **Aucune.** L'invité n'a pas besoin d'installer Node.js, Python, Docker, Git ou un compilateur C++. DevLink s'exécute de façon autonome.

---

## 2. Déploiement de l'Archive Autonome

1. Récupérer l'archive de distribution générée par le système de build :
   `PLPT-0.1.0-preview-windows-ucrt64-x64.zip` (ou package de release DevLink).
2. Extraire l'archive dans un répertoire utilisateur dédié, par exemple :
   `C:\DevLink\`
3. Vérifier la présence des fichiers essentiels dans `bin/` :
   - `plpt-desktop.exe` (Application desktop complète DevLink)
   - `plpt-monitor.exe` (Moniteur CLI d'inspection locale)
   - `SDL3.dll` (Runtime graphique SDL3 3.4.20)
   - Bibliothèques d'exécution UCRT64 (`libstdc++-6.dll`, `libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libiconv-2.dll`).

---

## 3. Lancement de l'Application

### Lancement de l'Interface Graphique (Recommandé)
Double-cliquer sur `bin\plpt-desktop.exe` ou lancer via le terminal :
```cmd
cd C:\DevLink\bin
plpt-desktop.exe
```

### Lancement en Mode Moniteur Console
Pour inspecter rapidement les ports locaux et les processus sans interface graphique :
```cmd
cd C:\DevLink\bin
plpt-monitor.exe
```

---

## 4. Configuration Réseau et Pare-Feu Windows

Lors du premier lancement d'une session de partage :
- Windows Defender Firewall peut afficher une invite d'autorisation réseau pour `plpt-desktop.exe`.
- Accepter l'accès sur les **Réseaux privés (domestiques ou de travail)**.
- Ne pas désactiver le pare-feu global. DevLink réserve uniquement ses propres sockets dynamiques sur l'interface locale.

---

## 5. Procédure de Désinstallation

DevLink ne crée aucune clé dans le Registre Windows, n'installe aucun pilote et n'enregistre aucun service d'arrière-plan.
Pour désinstaller l'application :
1. Fermer toutes les instances en cours d'exécution de `plpt-desktop.exe`.
2. Supprimer simplement le dossier d'extraction (`C:\DevLink\`).
3. Les profils temporaires de session résidant dans `%TEMP%\devlink_browser` sont automatiquement purgés à la fermeture de l'application.
