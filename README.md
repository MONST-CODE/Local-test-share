# DevLink — poste desktop local (préversion de développement)

**C++20 · SDL3 · Dear ImGui · Windows x64 / Linux x64 selon toolchain disponible**

> **État important : le partage réel d'une application web n'est pas disponible.**
> Le backend WebView2 échoue volontairement fermé : il ne crée pas de navigateur,
> ne capture pas de page et n'injecte pas d'interactions. Le backend synthétique sert
> uniquement aux tests et à une démonstration explicitement demandée. Cette branche
> n'est pas une release LAN prête à distribuer.

DevLink réunit un poste desktop et un moteur local en cours de reconstruction pour
observer des services, contrôler des sessions et, à terme, partager temporairement
une application web locale. L'objectif est de ne pas transférer le dépôt source et
ne pas exposer le bureau ou le système de fichiers de l'hôte. Les objectifs produit
ne sont pas à confondre avec les fonctions qualifiées dans l'état actuel.

## État fonctionnel vérifiable

- **Moniteur local :** observation des processus/services et de leurs ports, avec
  sélection explicite. La découverte ne constitue pas une autorisation de partage.
- **Poste desktop :** interface SDL3/ImGui, design system, ressources de marque et
  commandes reliées aux composants moteur disponibles. Le smoke test headless ne
  constitue pas une qualification de fenêtre native sur Windows ou Linux.
- **Plan de contrôle :** hôte/invité, admission, défi-réponse et transport TLS 1.3
  PSK figurent dans le code et ont des preuves d'intégration loopback consignées.
  La qualification actuelle Windows n'est pas établie ; l'implémentation TLS est
  indisponible sur Windows et le démarrage échoue si TLS est requis mais absent.
- **Proxy HTTP :** un proxy applicatif existe, mais son transport LAN n'est pas
  présenté comme chiffré de bout en bout et il ne remplace pas le partage de pixels.
- **Navigateur et médias :** WebView2 opérationnel, capture de vrais pixels, flux
  média vers un invité et retour d'interactions restent non implémentés. Aucun mode
  de participation A/B/C n'est qualifié sur un parcours réseau complet.

## Plateformes et limites

- **Windows x64 / MSYS2 UCRT64 / Clang++ :** chaîne de référence définie par
  `build.bat`; pas de qualification native disponible dans l'environnement de
  continuation courant. TLS WebView2, icônes PE et fenêtre native doivent y être
  revalidés.
- **Linux x64 :** build distinct défini par `build.sh`. Les résultats GCC utilisés
  pour le diagnostic sont exploratoires et ne remplacent pas Clang++ >=18. La
  disponibilité d'un vrai navigateur de partage Linux n'est pas fournie.
- **DPI :** captures SDL logiciel aux facteurs typographiques 100/125/150/200 %
  disponibles sous `docs/reports/evidence/dpi-captures/`. Elles ne prouvent pas une
  transition entre moniteurs physiques.
- **LAN multi-machine :** aucun parcours hôte/invité sur deux machines n'est
  qualifié. Les tests loopback ne sont pas une preuve de connectivité physique.

## Construire et tester

Consulter [`docs/BUILD.md`](docs/BUILD.md) avant le build. Les commandes officielles
sont les scripts versionnés ; elles ne téléchargent les dépendances que sur demande.

```sh
./build.sh help
./build.sh deps --fetch
./build.sh doctor --verbose
./build.sh test debug --headless
./build.sh selftest
```

Build officiel Linux bloqué si Clang++ >=18 n'est pas installé. `--headless` est
un profil de test, pas une build desktop native distribuable. Pour Windows, utiliser
`build.bat help` dans un terminal Windows x64 puis suivre `docs/BUILD.md` ; les
commandes Windows ne sont pas qualifiées en exécutant un build croisé Linux.

## Branding et preuves

Le maître SVG et ses exports sont sous `assets/branding/`. Les limites et mesures
sont consignées dans `docs/reports/evidence/`; les rapports anciens restent datés
et ne remplacent pas une exécution courante. Voir aussi :

- [`docs/SECURITY.md`](docs/SECURITY.md) — garanties et risques résiduels ;
- [`docs/NETWORK.md`](docs/NETWORK.md) — transport et frontières réseau ;
- [`docs/QUICKSTART.md`](docs/QUICKSTART.md) — portée réelle de cette préversion ;
- [`docs/reports/FINAL_COMPLETION_AUDIT.md`](docs/reports/FINAL_COMPLETION_AUDIT.md) —
  audit de clôture et critères encore bloquants.
