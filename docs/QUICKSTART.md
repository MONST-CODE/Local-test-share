# PLPT 0.1.0-preview — installation portable et utilisation

## Ce que contient cette version

Un **moniteur local**, CLI et interface native. Il observe les ports/processus du
compte courant, ne connecte aucun invité et ne démarre aucun partage. WebView2,
invitations, transport chiffré, cinq testeurs et nettoyage des profils sont absents.

## Construire et installer — Windows x64

Les prérequis de compilation ne concernent que le développeur : MSYS2 UCRT64 et
son `clang++.exe` exclusif, voir [BUILD.md](BUILD.md). Aucun SDK/Visual Studio requis. Après `build.bat deps --fetch`, exécuter `build.bat package`.
Le script recompile Release, teste puis crée :

```text
out\windows\packages\PLPT-0.1.0-preview-windows-ucrt64-x64.zip
```

1. Extraire l'archive dans un dossier appartenant à votre compte, par exemple
   `%LOCALAPPDATA%\Programs\PLPT-Preview`. Aucun droit administrateur requis.
2. Conserver **tout** le dossier `PLPT`, notamment les DLL à côté des EXE et les
   licences. Ne pas déplacer seulement `plpt-desktop.exe`.
3. Ouvrir `PLPT\bin\plpt-desktop.exe`, ou lancer `plpt-monitor.exe --help` depuis
   un terminal. Ne pas désactiver SmartScreen/antivirus si le binaire est bloqué.
4. Pour retirer la preview : fermer ses processus puis supprimer **ce dossier
   d'extraction uniquement**. Aucun service, règle de pare-feu ou désinstalleur ajouté.

Ce ZIP de développement (profil UCRT64, DLL GNU/Mingw vérifiées et livrées) n'est pas signé. Aucun installateur NSIS/MSI n'est livré
par la migration. L'exécution CI n'est pas une qualification SmartScreen, poste
propre, installation/désinstallation complète, DPI/IME ou approbation de redistribution du runtime GNU/SDL.

## Linux

`./build.sh package --native-ui` produit
`out/linux/packages/PLPT-0.1.0-preview-linux-x64.tar.gz`. Extraire, conserver la
bibliothèque SDL dans `PLPT/bin`, puis lancer `PLPT/bin/plpt-desktop` sous X11.
Le runtime glibc/libstdc++/X11 du système reste nécessaire ; ce n'est pas un binaire
universel pour toute distribution. Le profil dummy sandbox n'est pas distribuable.

## Utiliser le moniteur

- « Détection locale » affiche les sockets et leur attribution probable.
- Filtrer par adresse, port ou nom ; cliquer une ligne pour l'inspecter.
- « Sélection locale » et « Marquer protégé » sont des états locaux en mémoire.
- « Actualiser » relit les tables ; une erreur reste visible et invalide la sélection.
- Un PID inconnu n'est pas une raison de lancer systématiquement en administrateur.

Pas de Docker, Node, Python ou navigateur supplémentaire pour **utiliser le moniteur**.
Aucune promesse d'absence d'écriture système/cache/dump, ni d'effacement physique.

## Documentation

L'archive contient BUILD, SECURITY, QUICKSTART, l'inventaire de dépendances et les
notices. Les rapports et preuves complets restent dans le dépôt :
https://github.com/MONST-CODE/Local-test-share . La licence PLPT reste à décider par
son propriétaire avant diffusion ; les conditions de redistribution des runtimes
s'appliquent indépendamment des licences de notre code.
