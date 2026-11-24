# ADR-015 — Identité publique DevLink, version unique et identifiants internes stables

**Statut :** accepté (17 novembre 2026)
**Contexte :** mandat §16 (nettoyage du renommage PLPT → DevLink) et §16 « centraliser la
version produit ». Au commit `1e12c08`, quatre versions coexistaient : `RELEASE_NOTES.md`
annonçait 1.0.0, `--version` répondait 0.2.0-preview, le packaging nommait
`PLPT-0.1.0-preview-*.zip` et le manifeste Windows portait `0.1.0.0` sous l'identité
`PLPT.LocalMonitor.Preview`.

## Décision

1. **Identité publique renommée, de façon contrôlée, pas par remplacement aveugle :**
   - exécutables livrés `devlink-desktop`, `devlink-monitor`, `devlink-demo-service` ;
     exécutables de test `devlink-unit-tests`, `devlink-platform-tests`,
     `devlink-fuzz-envelope` ;
   - racine de package `DevLink/`, archives `DevLink-<version>-<plateforme>` ;
   - manifeste Windows : identité `DevLink.Desktop`, titre de fenêtre et messages CLI
     publics issus de `engine/core/product.hpp` ;
   - notices CI, logs de build et documentation utilisateur alignés.

2. **Version en source unique :** `engine/core/product.hpp` expose
   `devlink::kProductVersion`. `build.sh` la relit (`product_version()`), `build.bat` la
   relit (`:product_identity`) et en dérive le quadruplet Windows `majeur.mineur.patch.0`
   injecté dans le gabarit `packaging/windows/app.manifest` (fichier rendu dans `out/`,
   jamais le gabarit). La sonde de build (`tests/build/toolchain_probe.cpp --manifest
   <exe> --expect-version <quadruplet>`) fait échouer le link si le quadruplet embarqué
   dans le PE diverge de la source unique. Un README ou un ZIP ne peuvent donc plus
   diverger silencieusement du `--version`.

3. **Identifiants internes volontairement NON renommés :**
   - namespace C++ `plpt` et noms de types publics utilisés par les tests : un refactor
     de namespace n'apporte rien à l'utilisateur et casserait 37 cas unitaires et
     12 cas d'intégration sans bénéfice ; le mandat l'autorise explicitement ;
   - magie de protocole `0x504C5054` ('PLPT') et numéros de version de protocole :
     identifiants de fil, une modification casserait la compatibilité binaire ;
   - nom de paramètre/cookie `plpt` du proxy : artefact de protocole observé par des
     navigateurs réels, pas une identité de marque.
   Ces identifiants sont documentés comme tels dans `engine/core/product.hpp`
   (`kProtocolMagicStable`) afin qu'aucune future passe de renommage ne les touche par
   accident.

4. **Variables d'environnement :** noms publics `DEVLINK_*`
   (`DEVLINK_MSYS2_ROOT`, `DEVLINK_CXX`, `DEVLINK_CC`, `DEVLINK_EXTRA_CXXFLAGS`,
   `DEVLINK_EXTRA_LDFLAGS`, `DEVLINK_EXPECT_FAILURE`). Les anciens noms `PLPT_*` restent
   honorés pendant une release avec un avertissement, pour ne pas casser les
   environnements existants. Sous Windows, l'injection de drapeaux compilateur/lien
   reste **refusée** sous les deux noms (surface d'attaque du build) : seul
   `DEVLINK_MSYS2_ROOT` choisit l'installation.

## Conséquences

- Toute modification de version se fait en un seul endroit et se propage aux archives,
  au manifeste et aux rapports ; la divergence est détectée au link, pas en revue.
- Le code moteur garde son vocabulaire interne stable ; la marque n'apparaît que là où
  un utilisateur la lit.
- Le gabarit de manifeste rend le fichier `packaging/windows/app.manifest` non
  exécutable tel quel : il faut passer par `build.bat`, ce qui est le chemin normal et
  unique de production des exécutables Windows.
