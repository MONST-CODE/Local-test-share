# Qualification Windows native MSYS2 UCRT64

## Statut actuel : `BLOCKED_BY_ENVIRONMENT` pour le worktree courant

Date de rédaction selon la session : 2026-11-13 UTC. Le conteneur de cette
continuation est Linux x86_64 ; `cmd.exe`, MSYS2 UCRT64, clang++, WebView2 Runtime et
matériel Windows ne sont pas disponibles. Le build/test GCC exploratoire n'est pas
une qualification Windows.

Des rapports suivis dans ce checkout décrivent des runs Windows réussis, mais les
identifiants cités appartiennent à d'autres commits/branches. Un contrôle GitHub
(`gh run view 43607807540`) a confirmé une exécution `success` sur la branche
`arena/1a188065-local-test-share`, HEAD `b33f4a1a...`, pas le worktree audité ici.
Ces résultats historiques restent valides uniquement pour leur source/commit
identifiés. Ils ne valident ni le diff courant, ni WebView2, ni le partage réel.

## Environnement requis

- Runner Windows x64 natif `windows-2022`.
- MSYS2 UCRT64 selon les pins de `.github/workflows/build.yml` et
  `docs/BUILD.md`; clang++ exact à `%PLPT_MSYS2_ROOT%\ucrt64\bin\clang++.exe`.
- Versions/empreintes des paquets SDL3, ImGui, binutils, compiler-rt et des runtime
  WebView2 consignées dans le log; licences livrées avec le package.
- Le code actuellement présent ne construit pas un Browser Worker opérationnel ni
  un transport TLS Windows. Leur présence d'un Runtime/SDK WebView2 seul ne doit pas
  faire basculer la capacité en PASS.

## Commandes à exécuter sur le commit candidat

Depuis un `cmd.exe` normal, après checkout du SHA exact testé :

```bat
call build.bat deps --fetch
call build.bat doctor --verbose
call build.bat ide-check
call build.bat debug
call build.bat test debug --native-ui
call build.bat release
call build.bat test release --native-ui
call build.bat selftest
call build.bat clean
call build.bat package
```

Puis extraire l'archive sous un chemin vierge hors du dépôt et hors du PATH MSYS2,
comparer son manifeste/hash, lancer le CLI et l'UI, et vérifier les dépendances
runtime. Ne pas supprimer le contenu de `releases/` pour « nettoyer » un test.
Conserver les logs complets en artifact attaché au SHA et indiquer séparément les
étapes `SKIP` (ASan/libFuzzer conditionnels dans l'environnement UCRT64).

## Critères de qualification produit additionnels

Build/package Windows ne signifie pas partage réel. Avant de clore F04–F19, le même
SHA doit aussi réussir :

- WebView2 Runtime réel, navigation vers une fixture locale, preuve de DOM et pixels ;
- capture surface dédiée, frame reçue/décodée/affichée ;
- clic, saisie, profils A isolés, bail B exclusif et refus serveur C ;
- TLS 1.3 avec identité mutuelle, tests négatifs et aucun fallback clair ;
- cycle démarrage/rollback/arrêt/révocation/profil sans résidu ;
- ressources PE des binaires distribués, fenêtre/taskbar, DPI PerMonitorV2 physique ;
- qualification LAN avec une machine invitée distincte, enregistrée dans
  `REAL_LAN_QUALIFICATION.md`.

### Résultat de la continuation présente

Aucun code de sortie Windows n'a été produit par cette continuation : `NOT RUN`.
La configuration/script CI existant apporte un chemin de qualification natif à
exécuter sur le SHA candidat, mais ne prouve pas que le SHA courant a passé. Les
résultats de ce fichier sont donc `BLOCKED_BY_ENVIRONMENT`, pas PASS.
