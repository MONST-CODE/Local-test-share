# PLPT 0.1.0-preview — archive portable de développement

**Moniteur local uniquement : aucun partage, invité ou transport LAN.**

- Windows x64 : ouvrir `bin/plpt-desktop.exe` ou lancer `bin/plpt-monitor.exe --help`.
- Linux x64/glibc/X11 : ouvrir `bin/plpt-desktop` ou lancer `bin/plpt-monitor --help`.
- Conserver les bibliothèques à côté des exécutables. Ne pas copier uniquement l'EXE.
- Aucun service, élévation automatique ou règle de pare-feu. Aucun installateur signé.
- Pour retirer la preview : fermer ses processus et supprimer uniquement le dossier
  dans lequel **vous avez extrait cette archive**.

## Documentation incluse

- [Installation et utilisation](docs/QUICKSTART.md)
- [Build direct et prérequis](docs/BUILD.md)
- [Limites de sécurité](docs/SECURITY.md)
- [Inventaire des dépendances](docs/DEPENDENCIES.md)
- [Versions, hashes et ABI](docs/reports/BUILD_DEPENDENCIES.md)
- [SDL3](licenses/SDL3.txt), [Dear ImGui](licenses/ImGui.txt) et
  [notices incorporées](licenses/Bundled-notices.txt)

Les rapports complets de qualification et les logs sont dans le checkout source,
sous `docs/reports/`, pas dans cette archive compacte :
https://github.com/MONST-CODE/Local-test-share .

La licence PLPT reste à décider par son propriétaire. Les conditions de
redistribution des composants/runtimes restent applicables. Ce package n'est pas
une V1 de partage prête à être déployée chez cinq invités.
