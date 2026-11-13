# Preuves de développement — 2026-11-13

Ces fichiers sont des sorties d'exécution, pas des exemples rédigés à la main.
La capture PNG vient du BMP produit par `plpt-desktop`, sans retouche. Le rapport
principal explique les limites : [VALIDATION.md](../../VALIDATION.md).

- `environment.log`, `compiler.txt` : versions, branche/base, hashes maître/dépendances.
- `configure-*.log`, `build-*.log` : configurations et compilations. Les `*-final`
  sont les rebuilds UI après correction ; les logs initiaux restent disponibles.
- `tests-debug.log`, `tests-release.log`, `tests-asan.log` : 5/5 entrées CTest actuelles.
- `tests-ui.log`, `tests-ui-asan.log` : 7/7 actuelles, erreurs attendues comprises.
- **`tests-ui-asan-initial.log`** : 6/7, véritable fuite du chemin SDL_SaveBMP,
  conservée même après correction. Ne pas confondre ce fichier avec le résultat final.
- `build-debug-initial.log` : premier échec de compilation stricte, avant correction des tests.
- `windows-syntax.log` : succès syntaxique de 16 unités, aucune exécution Windows.
- `windows-syntax-initial.log`, `zig-include-probe.log` : tentatives échouées, non masquées.
- `windows-api-syntax.log` : vide parce que le premier check partiel de trois unités
  réussissait sans diagnostic ; le log complet actuel est `windows-syntax.log`.
- `headers.log`, `worker-repeat-asan.log`, `fuzz.log` : contrôles additionnels exécutés.
- `monitor-live.log`, `desktop-capture.log`, `desktop.png` : résultat réel des applications.
- `wrapper-*.log`, `reject-gcc.log` : succès du wrapper Release et refus attendus.
- `install-*.log`, `package-*.log`, `stage-ui-smoke.log` : packaging technique Linux,
  comparaison du binaire et smoke depuis le staging ; pas un installateur Windows.
- `source-inputs.sha256` : hashes des sources, scripts et métadonnées de build présents
  à la clôture ; les logs ne sont pas identifiés seulement par le HEAD initial du dépôt.
- `SHA256SUMS` : intégrité locale des preuves (hors ce fichier lui-même). Pas une
  signature de release, un audit externe ou une garantie de reproductibilité binaire.

Les chemins absolus dans les logs sont ceux du sandbox. Les temps CTest sont des
durées de tests de développement, pas des performances de streaming mesurées.
Les binaires, archives, corpus fuzz et caches volumineux restent hors Git sous `out/`
ou dans le cache d'outils. Ne pas y déposer de secret, jeton, profil ou `.env` réel.
