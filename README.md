# DevLink — poste desktop local (préversion de développement)

**C++20 · SDL3 · Dear ImGui · Windows x64 / Linux x64 selon toolchain disponible**

> **État : le partage réel fonctionne sous Windows et il est qualifié sur une seule
> machine. Le test LAN sur deux machines physiques n'a pas encore été exécuté.**
> L'hôte exécute l'application dans un vrai navigateur (WebView2), envoie ses images
> par un canal TLS 1.3 propre à chaque invité, et l'invité pilote la page (souris,
> clavier, texte Unicode) selon le mode A/B/C, que l'hôte fait respecter. Preuve :
> `devlink-desktop-e2e-tests` lance les vrais exécutables hôte et invité ; le clic et
> la saisie faits dans la fenêtre invitée modifient le DOM de l'hôte (ADR-018). Tant
> que `docs/reports/REAL_LAN_QUALIFICATION.md` n'est pas exécuté sur deux machines,
> cette branche n'est pas une release LAN qualifiée.

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
- **Plan de contrôle :** hôte/invité, admission, défi-réponse et TLS 1.3 PSK
  (OpenSSL 3 UCRT64 épinglé sous Windows, chargé depuis le dossier de l'application).
  Le démarrage d'un partage est refusé si TLS est indisponible, sans repli en clair.
- **Navigateur réel (Windows) :** WebView2 hors écran, un profil InPrivate par
  contexte, allowlist d'origines appliquée aux navigations, redirections,
  sous-ressources, fenêtres et téléchargements. Le navigateur ne sort que par une
  garde réseau SOCKS5 qui n'ouvre que les `hôte:port` choisis : pas d'autre port
  local, pas de base de données, pas de Docker.
- **Partage :** images JPEG du compositeur, canal média TLS 1.3 PSK séparé et propre
  à chaque invité (clé émise sur le contrôle chiffré), décodage WIC, texture SDL3,
  retour souris/molette/clavier/texte. Le mode A donne des profils navigateur
  indépendants (le backend de l'application reste commun). Le mode B donne un
  contrôle exclusif transférable. En mode C, les entrées sont refusées côté hôte.
- **Proxy HTTP :** le module existe encore, mais il **ne fait plus partie du
  partage** : il donnerait aux invités les ressources web du projet (ADR-013/018).

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
- **LAN multi-machine :** pas encore exécuté. Le protocole reproductible (avec les
  commandes `devlink-desktop --host` / `--join`) se trouve dans
  `docs/reports/REAL_LAN_QUALIFICATION.md`. Les tests en boucle locale ne prouvent
  pas la connectivité physique.
- **Invité Linux :** non disponible (décodeur JPEG portable à intégrer). Hôte et
  invité : Windows 10/11 x64.

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
