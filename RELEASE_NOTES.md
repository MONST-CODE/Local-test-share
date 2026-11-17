# Notes de version — DevLink 0.2.0-preview

**Date de publication : 17 novembre 2026**
**Version : 0.2.0-preview** (source unique : `engine/core/product.hpp`)
**Plateformes : Windows 10/11 x64 (MSYS2 UCRT64), Linux x64 (X11)**
**Architecture : C++20 / SDL3 / Dear ImGui**

---

## Ce que cette version EST

Une préversion de travail du poste de commande DevLink. Le socle réseau (plan de
contrôle hôte/invité, découverte LAN, proxy applicatif à jeton, registre
d'observation, messagerie de session) est implémenté et couvert par des tests
automatisés ; l'interface desktop est branchée sur l'état réel de ce socle.

Contenu vérifié par la chaîne de build et de tests (`build.bat` / `build.sh`,
commande `test`) :

- moniteur local d'observation passive (processus, sockets TCP/UDP, origines probables) ;
- session hôte transactionnelle : code d'invitation, approbation explicite de chaque
  invité, révocation, reprise de place par secret, compteurs réels ;
- client invité : authentification par code, attente d'autorisation, services publiés,
  messagerie relayée par le moteur ;
- annonce/écoute LAN multicast + repli broadcast + cible loopback explicite ;
- proxy HTTP strict : jeton retiré côté amont, cookie déposé uniquement sur réponse
  finale ≥ 200, 502 explicite si l'amont est injoignable ;
- suite unitaire et d'intégration exécutée à chaque build (`devlink-unit-tests`,
  `devlink-platform-tests`), sondes d'indépendance des en-têtes, auto-tests du script
  de build lui-même.

## Ce que cette version N'EST PAS (et n'affiche jamais comme acquis)

- **Pas de transport chiffré standard.** Les sessions circulent en TCP clair tant que
  TLS 1.3 avec authentification des pairs n'est pas intégré (mandat §24). L'interface
  l'affiche explicitement (« TCP clair — TLS non intégré ») et aucun écran, log ou
  rapport ne parle de canal sécurisé.
- **Pas de partage applicatif distant complet.** Le moteur WebView2 n'est utilisé que
  s'il est réellement disponible ; à défaut l'interface affiche « moteur navigateur
  indisponible » et bloque le démarrage d'un partage réel. Les trames synthétiques
  restent réservées aux tests et au mode `--demo-synthetic-browser`, signalé en
  permanence à l'écran.
- **Pas de chaîne capture → encodage → transport → décodage → texture SDL3 validée de
  bout en bout.** L'écran « Vue partagée » le dit au lieu de montrer une image factice.

## Identité et noms

- Exécutables livrés : `devlink-desktop`, `devlink-monitor`, `devlink-demo-service`
  (tests : `devlink-unit-tests`, `devlink-platform-tests`, `devlink-fuzz-envelope`).
- Racine de package : `DevLink/` ; archives :
  `DevLink-0.2.0-preview-windows-ucrt64-x64.zip`, `DevLink-0.2.0-preview-linux-x64.tar.gz`.
- Manifeste Windows : identité `DevLink.Desktop`, quadruplet `0.2.0.0` calculé depuis la
  version unique et vérifié dans le PE par la sonde de build.
- Identifiants internes volontairement stables (décision ADR-015) : namespace C++
  `plpt`, magie de protocole `0x504C5054`, nom de paramètre/cookie `plpt`. Ce sont des
  identifiants de fil, pas une identité publique.
- Variables d'environnement publiques : `DEVLINK_MSYS2_ROOT`, `DEVLINK_CXX`,
  `DEVLINK_CC`, `DEVLINK_EXTRA_CXXFLAGS`, `DEVLINK_EXTRA_LDFLAGS`,
  `DEVLINK_EXPECT_FAILURE`. Les anciens noms `PLPT_*` restent honorés une release, avec
  un avertissement ; sous Windows les knobs d'injection de drapeaux restent refusés.

## Installation

Voir `docs/QUICKSTART.md` et `docs/INSTALLATION.md` : extraire l'archive complète dans
un répertoire utilisateur (aucun droit administrateur), lancer `bin/devlink-desktop.exe`
ou `bin/devlink-monitor.exe --help`. Ne pas déplacer un exécutable isolé.
