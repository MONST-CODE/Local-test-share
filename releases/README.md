# Publications DevLink

Ce répertoire reçoit les archives produites par `./build.sh package`
(`DevLink-<version>-linux-x64.tar.gz`) — jamais écrites directement : le script
écrit `<archive>.partial`, vérifie l'intégrité de bout en bout et publie le
SHA-256 dans `<archive>.sha256`, puis renomme atomiquement. Un lecteur ne voit
donc jamais une archive tronquée, et une archive déjà publiée dont l'empreinte
diffère n'est jamais écrasée silencieusement (échec explicite).

Vérifier une archive reçue depuis ce dépôt :

    cd releases
    sha256sum -c DevLink-<version>-linux-x64.tar.gz.sha256

Convention : les binaires (`*.tar.gz`, `*.zip`) et les fichiers partiels
(`*.partial`) sont ignorés par Git ; seules les empreintes `*.sha256` et ce
fichier sont versionnés. `./build.sh clean` ne touche jamais ce répertoire —
c'est démontré par l'auto-test (`PASS clean-preserves-releases-proof`).

Ces archives de développement dépendent du glibc/libstdc++/X11 de l'hôte qui les
extrait (voir `packaging/README.md` dans l'archive et
`docs/reports/BUILD_DEPENDENCIES.md`).
