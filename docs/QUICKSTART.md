# PLPT 0.1.0-preview — guide de la preview

## Ce que contient cette version

Un **moniteur local**, en ligne de commande et, si compilée, une interface native.
Il observe les ports et processus du compte courant. Il ne connecte aucun invité,
ne démarre aucun partage et ne rend pas encore votre application à distance.

Les fonctionnalités finales WebView2, invitations, transport chiffré, cinq testeurs
et nettoyage des profils ne sont pas disponibles. Cette preview n'est pas une V1 LAN.

## Utilisation

- Ouvrir `plpt-desktop` : Détection locale affiche les sockets et leur attribution.
- Filtrer par adresse, port ou nom ; cliquer une ligne pour l'inspecter.
- « Sélection locale » et « Marquer protégé » ne sont que des états en mémoire.
- « Actualiser » relit les tables ; une erreur reste visible et invalide les sélections.
- `plpt-monitor --help` explique la sortie CLI. Sous Windows, un terminal UTF-8 est conseillé.
- Un PID inconnu n'est pas une erreur à corriger en lançant systématiquement en administrateur.

Pas de Docker, Node, Python ni navigateur supplémentaire pour utiliser **ce moniteur**.
Les outils de compilation sont réservés au développeur. Les dépendances réelles du
binaire Windows doivent encore être qualifiées sur une VM propre.

## Installateur Windows : état du livrable

La recette NSIS prévoit une installation par utilisateur dans
`%LOCALAPPDATA%\Programs\PLPT-Preview`, sans service ni modification du pare-feu.
Uninstall supprime les fichiers connus, pas un dossier utilisateur en récursif.
Une preview existante doit être désinstallée avant remplacement.

**L'installateur n'a pas été construit ni exécuté dans le sandbox Linux.** Aucune
signature, acceptation SmartScreen ou qualification Windows n'est revendiquée.
Ne pas désactiver les protections du poste pour l'installer. La distribution finale
nécessite une licence PLPT choisie par le propriétaire et la validation Windows.

## Documents techniques complets

Le dépôt et l'archive CPack contiennent `docs/BUILD.md`, `docs/SECURITY.md`, les
rapports et preuves datées. L'installateur compact, lorsqu'il sera qualifié, ne
contient que ce guide, l'inventaire des dépendances et les licences SDL/ImGui.
Référentiel : https://github.com/MONST-CODE/Local-test-share
