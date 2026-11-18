# DevLink — Audit de baseline (état initial vérifié)

**Date de l'audit :** 17 novembre 2026, 16:35–16:40 UTC
**Commit audité :** `1e12c08b2137808b65a50564b35f29a8dfac4971` (« feat:changement de l'UI ») — identique à `origin/main`
**Branche de travail :** `arena/125b0385-local-test-share` (aucun code modifié au moment de cet audit)
**Environnement d'audit :** Debian 12, `g++ (Debian 12.2.0-14+deb12u1) 12.2.0`, SDL3 épinglé `f00dc919` et Dear ImGui épinglé `c1aaac65` téléchargés et vérifiés SHA-256 par `./build.sh deps --fetch`
**Limite déclarée :** la chaîne de référence Windows (MSYS2 UCRT64 + clang++ 22) n'existe pas dans cet environnement ; `build.bat` et les exécutions Windows ne peuvent être validés ici que par CI. Les conclusions ci-dessous portent sur le code source et sur la compilation/exécution Linux.

Ce rapport remplace `docs/reports/INITIAL_FUNCTIONAL_AUDIT.md`, dont le mandat (§9.6) constate qu'il est daté d'un commit antérieur (`aa4ed7d`) et qu'il ne prouve pas l'état de `1e12c08`. Ce document ne contient **aucun** statut non associé à une commande, un log ou une limite vérifiable.

Preuves brutes : `docs/reports/evidence/2026-11-17-desktop-ui-compile-errors.txt`, `docs/reports/evidence/2026-11-17-baseline-tests.txt`.

---

## 1. Synthèse — l'état initial est cassé, pas « presque fini »

| # | Fait vérifié | Preuve |
|---|---|---|
| 1 | `app/desktop_ui/main.cpp` **ne compile pas** : 22 erreurs `error:` avec les drapeaux du produit | commande + log complet |
| 2 | La suite unitaire **échoue** : `unit: 37 cases, 20946 checks, 1 failures` | `./unit` exécuté |
| 3 | La suite d'intégration passe : `platform: 12 cases, 622 checks, 0 failures` | `./platform` exécuté |
| 4 | `secure_channel.cpp`, `webview2_backend.cpp`, `themes.cpp` ne sont dans **aucune** liste de sources de `build.sh` (l. 144–157) ni de `build.bat` (l. 58–62) | lecture des deux scripts |
| 5 | Le chiffrement du canal « sécurisé » est un **XOR maison** avec une sortie HMAC répétée | `secure_channel.cpp:157` et `:246` |
| 6 | Le backend navigateur émet des **frames synthétiques en dégradé**, y compris en repli | `browser.hpp` (`ManagedBrowserBackend::capture_frame`), `webview2_backend.cpp:144` |
| 7 | Aucun répertoire `assets/`, aucune icône `.ico`/`.svg`, aucun logo ; la ressource PE embarque seulement le manifeste | `find . -iname '*.ico' -o -iname '*.svg'`, `build.bat:582` |
| 8 | L'identité publique reste PLPT : `plpt-monitor.exe`, `plpt-demo-service.exe`, `plpt-desktop.exe`, staging `%STAGE%\PLPT\`, manifeste `PLPT.LocalMonitor.Preview`, 385 occurrences « PLPT » | `build.bat:602/605/627/880`, `packaging/windows/app.manifest` |
| 9 | Trois versions coexistent : `RELEASE_NOTES.md` annonce « 1.0.0 », `monitor --version` répond « DevLink 0.2.0-preview », le packaging nomme `PLPT-0.1.0-preview-…zip` | exécutions + lecture |

**Conséquence directe :** l'affirmation du rapport précédent (« 20 942 assertions validées sans échec ») est fausse pour le commit courant. Aucun livrable Windows n'a pu être produit depuis `1e12c08`, puisque la cible `plpt-desktop` ne compile pas.

---

## 2. Statuts par domaine (nomenclature du mandat)

| Domaine | Statut | Justification vérifiable |
|---|---|---|
| Compilation UI desktop | `NOT_IMPLEMENTED` | 22 erreurs ; la cible ne se lie jamais |
| Build (Linux) moteur + tests + outils | `IMPLEMENTED_AND_VERIFIED` | 4 binaires construits `-Wall -Wextra -Werror`, rc=0 |
| Build (Windows UCRT64) | `BLOCKED` | chaîne absente localement ; validation CI uniquement |
| Graphe de build complet | `PARTIALLY_IMPLEMENTED` | 3 `.cpp` non référencés ; en-têtes `secure_channel.hpp`, `webview2_backend.hpp`, `remote_frame.hpp`, `browser.hpp`, `design_system/*` absents des listes HEADERS |
| Suite unitaire | `PARTIALLY_IMPLEMENTED` (rouge) | 1 cas en échec : `secure_channel_confidentiality_and_anti_replay` (`test_main.cpp:991`) |
| Suite intégration plateforme | `IMPLEMENTED_AND_VERIFIED` | 12/622/0 |
| Transport sécurisé | `NOT_IMPLEMENTED` pour l'exigence §24 | XOR + HMAC maison ; pas de TLS ; pas d'authentification mutuelle démontrée ; `set_keys()` marque le canal établi sans handshake |
| Moteur navigateur réel | `NOT_IMPLEMENTED` | pas de `CoreWebView2` opérationnel ; frames synthétiques ; repli silencieux sur `ManagedBrowserBackend` qui synthétise aussi |
| Chaîne capture → encodage → transport → décodage → texture SDL3 | `NOT_IMPLEMENTED` | `remote_frame.hpp` sérialise en endianness native par `memcpy` : pas de format réseau portable, pas de validation de limites complète |
| Retour d'interaction (modes A/B/C) | `NOT_IMPLEMENTED` | `inject_input()` compte un événement sans l'injecter |
| Design system (thèmes) | `PARTIALLY_IMPLEMENTED` | `tokens.hpp` déclare des structures sans valeurs de palette ; `themes.cpp` non compilé ; `apply_to_imgui()` jamais appelé par un binaire construit |
| Typographie | `NOT_IMPLEMENTED` | `typography.hpp` ne fait que `SetWindowFontScale()` ; aucune famille chargée |
| Icônes | `NOT_IMPLEMENTED` | libellés textuels entre crochets (`icons.hpp`) |
| Logo / identité visuelle | `NOT_IMPLEMENTED` | aucun fichier vectoriel, aucune résolution PNG, aucun `.ico`, aucune ressource PE, aucune icône de fenêtre |
| Renommage PLPT → DevLink | `PARTIALLY_IMPLEMENTED` | textes partiels (« DevLink » dans `secure_channel.hpp`, `--version`), mais binaires, staging, manifeste et 385 occurrences PLPT |
| Packaging + `releases/` + SHA-256 | `NOT_IMPLEMENTED` | aucun répertoire `releases/` à la racine ; pas de vérification post-extraction sur ce chemin ; l'archive ne peut pas être produite puisque la compilation échoue |
| Rapport d'audit honnête | `REGRESSION` | le rapport précédent affirme des succès non reproduits au commit courant |

---

## 3. Les 22 erreurs de compilation, causes réelles

Commande exacte (dépendances épinglées, drapeaux du produit) :

```
g++ -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -fsyntax-only -I. \
    -I<sdl3 f00dc919>/include -I<imgui c1aaac65> -I<imgui c1aaac65>/backends \
    app/desktop_ui/main.cpp
```

| Ligne | Erreur | Contrat réel du moteur |
|---|---|---|
| 179, 492, 495 | `shared_ptr<const HostSnapshot>` n'a pas de membre `participants` | `monitor->view()` renvoie un pointeur intelligent : accès par `->`, et la liste des participants vient de l'instantané hôte, pas du `MonitorView` |
| 264 | `plpt::Monitor` n'a pas de membre `refresh_now` | `Monitor::request_refresh()` |
| 289, 294 | `ServiceCandidate` n'a pas de membre `process_name` | `candidate.process` est un `std::optional<ProcessRecord>` → `process->image_name` |
| 374, 375 | `plpt::PortAllocator` inexistant ; `allocator` non déclaré | `DynamicPortAllocator::acquire(TransportProtocol, IpFamily, BindScope)` → `Result<ReservedListener>` |
| 375, 378 | `plpt::IpFamily::V4` inexistant | `enum class IpFamily : std::uint8_t { Unknown, Ipv4, Ipv6 }` (`core/types.hpp:23`) ; `TransportProtocol { Unknown, Tcp, Udp }` (l. 24) |
| 394, 395 | `plpt::net::HostServerConfig` inexistant | `HostConfig` (`approval_required`, `control_port`, …) |
| 401 | `HostServer::start()` sans argument n'existe pas | `HostServer(IClock&)` puis `start(const HostConfig&)` → `Result<std::uint16_t>` |
| 404, 405 | `plpt::net::AnnouncerConfig` inexistant | `Announcer::Config` (type imbriqué), `Announcer(IClock&)`, `start(Config)` |
| 410 | `Announcer::start()` sans argument n'existe pas | idem |
| 536, 537 | `plpt::net::GuestClientConfig` inexistant | `GuestConfig` (`address: IpAddress`, `control_port`, …) |
| 549 | `GuestClient::connect_and_authenticate()` est **privé** | point d'entrée public : `GuestClient(IClock&)` + `start(GuestConfig)` |
| 569 | `GuestClient` n'a pas de membre `disconnect` | API publique réelle (`guest_client.hpp:129-137`) : `start(GuestConfig)`, `stop()`, `running()`, `snapshot()`, `wait_for(pred, timeout)`, `post_chat()`, `post_direct_message()`, `leave()` |
| 697 | `plpt::Error` n'a pas de membre `c_str` | `Error` expose des `std::string` (message/contexte), pas de C-string |
| 801 | opérandes de `?:` de types incompatibles | `view()` renvoie `shared_ptr<const MonitorView>` : ne pas mélanger avec un `MonitorView` par valeur |

Deux défauts supplémentaires, non bloquants pour le compilateur mais bloquants pour le mandat :

- `app/desktop_ui/design_system/components.hpp` : `status_pill()` passe `label.data()` (un `std::string_view`) à `ImGui::Button()`, qui attend une chaîne terminée par `\0` → lecture hors limites possible.
- `app/desktop_ui/design_system/components.hpp` : variable inutilisée dans `empty_state()` → `-Werror` dès que `themes.cpp`/`main.cpp` compilent avec les drapeaux du produit.
- `main.cpp` positionne `host_running = true` après des appels `start()` dont le `Result` n'est pas vérifié : démarrage non transactionnel, interdit par §12.
- L'écran de diagnostic de `main.cpp` annonce un « canal sécurisé » HMAC et un WebView2 actif : affirmation fausse au regard des §5 et §6 ci-dessous.

Le nombre total d'erreurs augmentera après correction de celles-ci (erreurs en cascade masquées) : ce compteur est un **plancher**, pas un aboutissement.

---

## 4. Graphe de build — ce qui manque

`build.sh` lignes 144–157 et `build.bat` lignes 58–62 listent explicitement leurs sources. Comparaison avec l'arbre réel :

| Fichier | Présent dans une liste ? | Conséquence |
|---|---|---|
| `engine/net/secure_channel.cpp` | non | jamais compilé par les scripts ; son test unitaire n'est exécuté que si on l'ajoute à la main |
| `engine/browser/webview2_backend.cpp` | non | `create_browser_backend()` non disponible dans les binaires produits par les scripts |
| `app/desktop_ui/design_system/themes.cpp` | non | `ThemeManager` : symboles non définis à l'édition de liens dès que l'UI compile |
| en-têtes `secure_channel.hpp`, `webview2_backend.hpp`, `remote_frame.hpp`, `browser.hpp`, `design_system/*` | absents de HEADERS | contrôle d'existence inopérant pour ces fichiers |

Vérification croisée faite en construisant à la main une liste élargie : `tests/unit`, `tests/integration`, `app/host_shell`, `app/demo_service` compilent et se lient avec `-Wall -Wextra -Werror` **une fois ces sources ajoutées** — ce qui prouve que le moteur est sain et que le trou est bien dans les scripts.

---

## 5. Transport « sécurisé » — non conforme §9.2 / §24

- `secure_channel.cpp:157` : `ciphertext[i] = plaintext[i] ^ keystream.value()[i % kMacBytes]` avec `keystream = HMAC-SHA256(enc_key, sequence)`. Un flux de 32 octets répété en boucle sur tout le message : construction maison, réutilisation de keystream, aucune primitive AEAD, aucune négociation de clés standard.
- `secure_channel.cpp:246` : même construction côté réception.
- `set_keys()` public positionne `established_ = true` sans handshake ni preuve d'authentification mutuelle.
- Le constructeur à deux arguments `SecureChannel(SecureSessionKeys, bool)` **ne positionne pas** `established_`, alors que `set_keys()` le fait : incohérence qui fait échouer le test unitaire `secure_channel_confidentiality_and_anti_replay` (`seal()` renvoie « canal sécurisé non établi »). C'est la cause exacte de l'unique échec de la suite au commit audité.
- Rien de tout cela n'est branché sur le transport réel : `host_server`/`guest_client`/`proxy` communiquent en TCP clair.

**Décision de conformité :** ce module doit être retiré du chemin de production et remplacé par un transport standard (TLS 1.3 avec authentification des pairs) derrière une interface de transport, avec ADR versionné. Aucune primitive cryptographique maison ne sera développée.

---

## 6. Moteur navigateur — non conforme §9.1 / §21 / §22

- `webview2_backend.cpp` charge `CreateCoreWebView2EnvironmentWithOptions` par `GetProcAddress` mais ne construit pas l'environnement, le contrôleur et le `CoreWebView2` de façon opérationnelle ; `capture_frame()` produit un dégradé RGBA commenté « Rendu hors-champ du navigateur avec dégradé DevLink reflétant l'activité » (l. 144).
- `inject_input()` valide et incrémente `frame_counter` sans injection réelle.
- `browser.hpp` : `ManagedBrowserBackend::capture_frame()` synthétise une trame 128×64 en dégradé dont les couleurs dépendent de `has_navigated` — un compteur et un dégradé tiennent lieu de capture.
- `create_browser_backend()` retombe sur ce backend synthétique si WebView2 manque : **repli silencieux interdit** en production. L'UI doit afficher « moteur navigateur indisponible » et bloquer le démarrage d'une session réelle.

---

## 7. Identité visuelle — non conforme §13–§15

- Aucun répertoire `assets/` ; aucun `.svg`, `.ico` ou `.png` de marque dans le dépôt (le seul PNG trouvé est une capture d'écran de preuve antérieure).
- `build.bat:582` génère `application.rc` avec **une seule** entrée : le manifeste (`1 24`). Aucune ressource `ICON`, donc aucune icône dans le PE, ni dans la fenêtre, ni dans l'explorateur.
- `packaging/windows/app.manifest` : `assemblyIdentity name="PLPT.LocalMonitor.Preview"`, `version="0.1.0.0"`.
- Le DPI est déclaré PerMonitorV2 dans le manifeste, mais aucune chaîne de rendu DPI (polices réelles, mise à l'échelle des textures) n'existe côté code : la conformité 100–200 % n'est pas démontrable.
- `design_system/tokens.hpp` ne contient que des déclarations de structures, aucune valeur de palette : DevDark/DevLight ne sont pas définis.
- `design_system/icons.hpp` : libellés textuels entre crochets.

---

## 8. Plan d'exécution retenu (ordre du mandat)

Chaque étape se termine par une compilation propre et l'exécution des tests concernés ; aucun statut `PASS` ne sera revendiqué sans log.

1. **Étape 1 — Rétablir un build vert et honnête.** Raccorder les sources manquantes aux deux scripts ; corriger les 22+ erreurs de `main.cpp` en s'adaptant aux contrats réels du moteur (aucun cast, aucun pragma, aucune fonction inventée) ; corriger `components.hpp` (terminaison de chaîne, variable inutilisée) ; rendre le démarrage hôte/invité transactionnel (vérification de chaque `Result`).
2. **Étape 2 — Identité publique DevLink.** Renommage des binaires, du staging, du manifeste, des noms d'archives, des textes visibles ; conservation des identifiants internes (namespace, protocole) selon §16 « à examiner avant de renommer » ; une seule version cohérente partout.
3. **Étape 3 — Design system réel.** Valeurs de palette DevDark/DevLight dans `tokens.hpp`, `themes.cpp` compilé et appliqué, typographie réellement chargée (fichiers de polices + plages de glyphs + rendu net), icônes vectorielles ImGui au lieu des crochets, gestion DPI 100–200 % mesurée.
4. **Étape 4 — Marque et ressources.** Logo `<-D->` reconstruit en vectoriel propre (chevrons de code, pas de pointes de flèche, pas de slogan dans l'icône applicative), exports multi-résolutions PNG, `.ico` multi-tailles, ressource PE `ICON` dans `build.bat`, icône de fenêtre SDL3. La référence bitmap fournie reste une direction artistique : jamais une source de calque ni un embedding tel quel.
5. **Étape 5 — Transport standard.** ADR de choix TLS 1.3 (ou équivalent audité) + interface de transport ; retrait du XOR maison du chemin de production ; tests d'authentification mutuelle, de confidentialité et d'anti-rejeu réels.
6. **Étape 6 — Partage applicatif réel.** Découverte/sélection de service, file d'approbation, backend WebView2 opérationnel avec profils isolés, format de frame réseau portable (tailles, version, ordre des octets, limites, validation), chaîne capture → encodage → transport authentifié → décodage → texture SDL3, retour d'entrée avec modes A/B/C appliqués côté serveur, proxy strict.
7. **Étape 7 — Séparation test/production.** Backends synthétiques réservés aux tests et aux builds de démonstration explicitement identifiés ; en production, absence de moteur = message explicite + blocage des sessions réelles.
8. **Étape 8 — Packaging.** Archives vers `releases/` à la racine via `.partial` + renommage atomique, SHA-256 publié, extraction vérifiée ; `clean` ne touche jamais `releases/`.
9. **Étape 9 — Rapport final.** Statuts PASS/FAIL/BLOCKED/PARTIAL par maillon, avec commande, log, capture ou limite pour chacun.

---

## 9. Étape 1 — build vert et honnête (vérifié le 17 novembre 2026, 17:05 UTC)

Preuve complète : `docs/reports/evidence/2026-11-17/etape1-chaine-locale-verte.txt`,
captures `desktop-ui-prod-engine-smoke.png` et `desktop-ui-demo-synthetic-smoke.png`.

Résultats obtenus sur la chaîne complète (`build.sh test debug --headless`, réplication
locale avec g++ ; la chaîne de référence clang/UCRT64 reste celle de la CI) :

- `BUILD SUCCESS mode=debug` ;
- `unit: 37 cases, 20954 checks, 0 failures` (le cas `secure_channel_confidentiality_and_anti_replay`
  passe à nouveau : le constructeur à deux arguments de `SecureChannel` a la même sémantique
  que `set_keys()` — canal établi) ;
- `platform: 12 cases, 622 checks, 0 failures` ;
- `PASS 33 independent native headers` + `PASS 6 independent UI headers` (nouvelle sonde) ;
- `desktop-smoke: 8 frames, snapshot=1, services=10, sharing=disabled` : le poste de commande
  s'exécute, observe réellement 10 services locaux et n'affiche aucun partage ;
- `TEST SUMMARY passed=15 skipped=0` — l'attente `ui-file-error` qui échouait était un défaut
  réel masqué par l'échec de compilation : `SDL_SaveBMP` ne produit pas le diagnostic
  `SDL_IOFromFile:` attendu. L'application ouvre/écrit/ferme désormais le flux elle-même
  (`SDL_IOFromFile`, `SDL_SaveBMP_IO`, `SDL_CloseIO`) avec un diagnostic nommé par appel.

Corrections de graphe de build : `engine/browser/webview2_backend.cpp` (groupe `BROWSER_SOURCES`,
lié au poste de commande et aux tests), `engine/net/secure_channel.cpp` (groupe
`MIGRATION_SOURCES`, **lié aux tests seulement** : la crypto maison ne doit pas entrer dans un
binaire livré), `app/desktop_ui/design_system/themes.cpp` (groupe `UI_SOURCES`, poste de
commande). Les en-têtes `remote_frame.hpp`, `webview2_backend.hpp`, `secure_channel.hpp` et les
six en-têtes `design_system/*` sont désormais sondés pour l'indépendance.

Interface : réécrite contre les contrats réels (`Monitor::view()/request_refresh()`,
`HostConfig`/`HostServer(IClock&)/start()`, `Announcer::Config` + `announce_targets()`,
`GuestConfig`/`GuestClient(IClock&)/start()`, `DynamicPortAllocator`, `ServiceProxy`,
`ParticipantRecord`, `to_string(Confidence|ShareState|ErrorCode)`), démarrage hôte
**transactionnel** (rollback complet au premier échec, `host_running` jamais positionné sans
confirmation du moteur), file d'approbation réelle (`approve`/`deny`), révocation réelle
(`revoke`), chat réellement relayé par le moteur, écoute LAN réelle (`DiscoveryListener`),
saisie manuelle via `inject_manual`. Aucun état « connecté/sécurisé/streaming » affiché sans
confirmation du moteur ; le transport annonce « TCP clair — TLS non intégré ».

Moteur navigateur : `WebView2Backend` n'est instancié que si `is_available()` ; le backend
synthétique n'est accessible que par `--demo-synthetic-browser`, avec avertissement stderr et
bandeau permanent à l'écran. Un garde `__has_include(<WebView2.h>)` distingue « SDK absent de
la compilation » de « runtime absent », sans repli silencieux (`kWebView2SdkCompiled`).
Sécurité des composants : `status_pill` n'utilise plus `string_view::data()` avec une API
attendant une chaîne terminée par zéro (plage `[begin, end)` explicite, aucune allocation).

## 10. Étape 2 — identité publique DevLink, version en source unique (mandat §16)

- `engine/core/product.hpp` : source unique (`kProductName`, `kProductVersion`,
  `kProductTagline`, magie stable `kProtocolMagicStable`). `--version`, titre de fenêtre,
  usage, manifeste Windows et noms d'archives en découlent ; `build.sh`
  (`product_version()`) et `build.bat` (`:product_identity`) relisent le fichier, aucune
  copie manuelle ne subsiste.
- Exécutables `devlink-desktop` / `devlink-monitor` / `devlink-demo-service`, tests
  `devlink-unit-tests` / `devlink-platform-tests`, fuzzer `devlink-fuzz-envelope`,
  racine de package `DevLink/`, archives `DevLink-<version>-<plateforme>`.
- Identifiants internes volontairement stables et documentés (`docs/ADR/ADR-015`) :
  namespace `plpt`, magie de protocole `0x504C5054`, cookie/paramètre `plpt`.
  Variables publiques `DEVLINK_*`, anciens `PLPT_*` honorés une release avec
  avertissement ; injection de drapeaux refusée sous les deux noms (Windows).
- `RELEASE_NOTES.md` réécrit honnêtement (0.2.0-preview : ce qui est vérifié, ce qui ne
  l'est pas). Vérification locale : `TEST SUMMARY passed=15 skipped=0`,
  unit 37/20954/0, platform 12/622/0 (log `out/linux/logs/20261117T171807Z-*`).

## 11. Étape 3 — identité visuelle réelle (mandat §13–§15, §17)

- **Marque vectorielle** : `assets/branding/devlink-icon-master.svg` (symboles
  `#dl-standard`, `#dl-compact` et variantes tuilées), `devlink-symbol.svg`,
  `devlink-logo.svg`, `devlink-wordmark.svg` (lettres en tracés, aucune police),
  `devlink-mono-dark.svg`, `devlink-mono-light.svg`, `BRAND_GUIDELINES.md`.
  Géométrie `<-D->` : chevrons de code arrondis (pas de pointes de flèche), tirets de
  séparation, D dominant, symétrie exacte x=120, aucun slogan dans l'icône.
  `DevLink-Brand-Reference.png` n'a servi ni de source bitmap ni d'embarquement.
- **Exports raster versionnés** : `devlink-{16,24,32,48,64,128,256}.png` et
  `devlink.ico` (7 entrées réelles : 16–128 en DIB 32 bpp + masque AND, 256 en PNG),
  produits par `tools/branding/export_icons.py` (rasteriseur vectoriel autonome, sans
  PIL/cairo/numpy ; hors build, jamais appelé par build.sh/build.bat). Le ICO est
  vérifié à l'export (`--verify`) et au build par la sonde `probe --ico`
  (contrôle positif) et `probe --ico <fichier renommé>` (négatif, selftest).
- **Ressources PE** : `packaging/windows/application.rc.in` (gabarit versionné :
  manifeste + `2 ICON` + `VERSIONINFO` au quadruplet dérivé de `product.hpp`) rendu par
  `build.bat :render_resources` ; objet de ressources propre au binaire applicatif
  (`desktop_application.o`), monitor/demo-service gardent l'objet manifeste seul.
  Après link, `probe --pe-icons devlink-desktop.exe --expect-icons 7` lit les entrées
  `RT_GROUP_ICON`/`RT_ICON` du PE : l'icône embarquée est prouvée, pas supposée.
  Non exécutable ici (pas de Windows) : vérification CI/manuelle requise — statut
  `IMPLEMENTED_BUT_NOT_VERIFIED` côté Windows, vérifié côté Linux (`--ico`).
- **Icône de fenêtre runtime** : RGBA 64×64 embarqué
  (`design_system/window_icon_data.{hpp,cpp}`, généré depuis le maître) posé par
  `SDL_SetWindowIcon` ; sous Windows l'icône PE prime.
- **Typographie réelle** : `design_system/typography.{hpp,cpp}` — famille vectorielle
  chargée depuis le disque (police OFL embarquée si présente, sinon Segoe UI / DejaVu /
  Noto / Liberation du contexte), glyphes latin-1 + œ + ponctuation typographique,
  niveaux hero/section/card/body/small/code en tailles réelles multipliées par
  l'échelle mesurée `SDL_GetWindowDisplayScale`, rechargement de l'atlas au changement
  d'échelle (backend `RendererHasTextures`). Aucun `SetWindowFontScale` ne subsiste.
  Échec de chargement = repli ImGui **affiché** aux diagnostics (famille, chemin, note).
- **Icônes vectorielles d'interface** : `design_system/icons.hpp` — 18 glyphes dessinés
  à l'ImDrawList (grille 24, traits arrondis, épaisseur suit l'échelle), marque `<-D->`
  redessinée depuis la grille 240 du maître (`brand_mark`), réservation de layout
  explicite, libellés accessibles (`icons::label`), badges de statut avec glyphe.
- **Captures réelles** : `docs/reports/evidence/2026-11-17-etape3-ui-reelle.png` (tableau de bord
  complet, police système chargée, marque et glyphes visibles, bandeaux honnêtes
  « MOTEUR NAVIGATEUR INDISPONIBLE » / « TCP CLAIR - TLS NON INTEGRE ») et
  `docs/reports/evidence/2026-11-17-etape3-topbar-marque.png`.
- Vérification : `./build_local_tmp.sh test debug --headless` →
  `TEST SUMMARY passed=15 skipped=0`, unit 37/20954/0, platform 12/622/0 ;
  selftest → `brand-ico` PASS, `renamed-ico` rejeté, `SELFTEST PASS` ;
  détails dans `docs/reports/evidence/2026-11-17-etape3-identite-visuelle.txt`.

## 12. Étape 4 — transport versionné : TCP clair déclaré + TLS 1.3 PSK réel (ADR-016), 17 novembre 2026

- **Non-conformité traitée** : `engine/net/secure_channel.cpp` (XOR + HMAC
  SHA-256 maison, magie `0x444C4E4B`) était le seul canal « sécurisé » —
  confidentiel mais malléable, sans authentification de pair, sans pile
  maintenue. Le mandat §24 exige TLS 1.3 avec authentification du pair.
- **Interface versionnée** : `engine/net/transport.hpp`
  (`kTransportInterfaceVersion=1`) — `ITransportChannel` (send/receive/close/
  **info**), `ITransportEndpoint` (accept/port/close), `ITransportFactory`
  (listen/connect/**available**/**unavailability_reason**), `TlsPolicy`
  (min=max=TLS 1.3 par défaut, politique incohérente refusée à la construction).
  Tout canal se déclare : le clair dit `"tcp clair"` / `peer_authenticated=false`
  — jamais d'état sécurisé prétendu sans poignée aboutie.
- **TLS 1.3 PSK réel** : `engine/net/transport_tls.cpp` — OpenSSL chargé par
  dlopen (pas d'en-têtes à la compilation ; sandbox Debian 12 sans libssl-dev),
  symboles liés un à un avec raison explicite en cas de manque ; PSK externe
  identité+secret ≥16 o, clé = SHA-256(`DevLink TLS13 PSK v1|id|secret`), suite
  unique **épinglée des deux côtés** `TLS_AES_128_GCM_SHA256` (sans épinglage,
  OpenSSL préfère AES-256-GCM-SHA384, le serveur jette la session PSK SHA-256
  et retombe sur certificats — cause racine vérifiée contre les sources 3.0.20,
  corrigée, re-testée) ; macros d'ABI traitées (`SSL_CTX_ctrl` 123/124,
  `SSL_get_current_cipher`+`SSL_CIPHER_get_name`, `SSL_SESSION_set1_master_key`).
  Échec de poignée ⇒ `Unauthorized`, aucun canal, aucun repli clair.
- **Windows** : fabrique TLS `available()=false` avec raison (« runtime TLS non
  câblé sur Windows (Schannel/OpenSSL MSYS2 : ADR-016) ») —
  `IMPLEMENTED_BUT_NOT_VERIFIED` depuis Linux, décidé et documenté dans l'ADR.
- **Pré-basculement** : `TRANSPORT_SOURCES` liés AUX TESTS SEULEMENT (build.sh
  + build.bat), même régime que `MIGRATION_SOURCES` ; le basculement
  `host_server`/`guest_client` + retrait de `secure_channel` est l'étape 5.
- **6 nouveaux cas d'intégration** (platform 12→18 cas, 622→670 checks) :
  clair auto-déclaré ; boucle TLS 1.3 authentifiée des deux côtés
  (`TLSv1.3`/`TLS_AES_128_GCM_SHA256`/`peer_authenticated=true`) ; mauvais
  secret refusé des deux côtés ; downgrade 1.2 refusé (fabrique + serveur) ;
  **oracle de fil** (relais octet-par-octet : marqueur jamais en clair sur le
  fil — chiffrement effectif prouvé, pas déclaré) ; honnêteté de disponibilité.
  Auto-test négatif `tls-unavailable-proof` (build.sh selftest) : avec
  `DEVLINK_TLS_LIBRARY_PATH=/does/not/exist`, raison explicite, `Unsupported`,
  cas TLS en **SKIP** (14 cas, 0 échec) — SKIP ≠ PASS, suite verte et honnête.
- **Sonde d'étape 3 RECONSTRUITE** : la recréation d'environnement avait perdu
  les extensions de `tests/build/toolchain_probe.cpp` (`--ico`, `--pe-icons`,
  `--expect-version`) alors que build.sh/build.bat les exigent — restauration
  réelle, pas cosmétique : parseur ICO octet par octet (DIB32 taille exacte,
  PNG IHDR BE RGBA8, fin de fichier == fin des charges, tailles requises
  16–256) vérifié sur le vrai `devlink.ico` (7 entrées, RC=0) et sur des
  contrefaçons (RC=1) ; parseur PE32+ pur C++ (ressources, GRPICONDIR coherent,
  VS_FIXEDFILEINFO) validé depuis Linux sur **PE synthétiques**
  (`tests/build/synthetic_pe.py`) : bon PE RC=0, groupe incohérent / RT_VERSION
  absent / quad divergent / ELF renommé RC=1. Bug de racine (RVA absolu vs
  offset relatif) trouvé par cette validation et corrigé avant usage en barrière.
  `window_icon_data.hpp` ajouté à `UI_HEADERS` (6→7 en-têtes UI indépendants).
- Vérification : `./build_local_tmp.sh test debug --headless` → `TEST SUMMARY
  passed=15 skipped=0`, unit 37/20954/0, platform 18/670/0, 34 en-têtes natifs
  + 7 UI indépendants ; selftest → `SELFTEST_EXIT=0` (brand-ico PASS, négatifs
  by-design, tls-unavailable-proof PASS) ; détails dans
  `docs/reports/evidence/2026-11-17-etape4-transport-tls13.txt`.

## 13. Étape 5 — plan de contrôle basculé sur TLS 1.3, secret prouvé et jamais transmis (ADR-017), 18 novembre 2026

- **Non-conformités traitées** : (1) `host_server`/`guest_client` parlaient TCP
  clair en production alors qu'ADR-016 avait livré le transport TLS 1.3 — l'étape
  4 était volontairement non branchée ; (2) le protocole 1.2 faisait transiter
  **le code d'invitation en clair** dans `GuestHello` et **le secret de reprise en
  clair** à la reconnexion : un observateur du LAN capturait le droit d'entrer ;
  (3) `engine/net/secure_channel.cpp` (XOR + HMAC maison) restait lié aux tests.
- **Piège évité** : l'ébauche de basculement (commit `74322c3`) faisait porter le
  PSK par une notice **en clair**. Un PSK transmis en clair ne protège contre
  rien : l'observateur qui lit la notice déchiffre tout le canal. Annoncer
  « TLS 1.3 » dans cet état aurait été un état sécurisé fictif. D'où ADR-017.
- **Protocole 1.3** : `AuthChallenge = 17` (nonce public 32 o, entropie OS),
  `AuthProof = 18` (HMAC-SHA256 du code et/ou du secret de reprise), et
  `TlsUpgrade = 16` réduite à des données publiques (identité PSK, nature du
  secret, nonce du défi). `validate_header` n'autorise un participant nul que
  pour `Hello`, `Auth`, `AuthChallenge`, `AuthProof`, `TlsUpgrade` ; la séquence
  reste obligatoire — aucune de ces trames n'est rejouable.
- **Aucune crypto maison** : `engine/core/mac.{hpp,cpp}` implémente HMAC
  (RFC 2104) au-dessus du fournisseur d'empreintes de l'OS déjà en place
  (`digest.hpp` : CNG / AF_ALG / libcrypto). Construction vérifiée contre les
  vecteurs **RFC 4231** 1, 2, 3, 6, 7 + cas clé/message vides, avec contrôle
  indépendant (`hmac` de Python). Le secret de transport est
  `hex(HMAC(secret, "DevLink TLS13 PSK v2|" || nonce || session || identité))`,
  dérivé **des deux côtés**, jamais transmis, passé comme PSK à la fabrique
  ADR-016 (TLS 1.3, `TLS_AES_128_GCM_SHA256`, authentification mutuelle).
- **Chemin clair historique BLOQUÉ** : un `Hello` portant encore `invite_code` ou
  `resume_secret` est refusé `ProtocolViolation` (« secret transmis en clair
  refusé »). Côté invité, une admission non précédée d'un upgrade est refusée
  quand `tls_required` est vrai. Échec de poignée de main ⇒ déconnexion, jamais
  de repli clair silencieux.
- **Ordre réel** : Hello → AuthChallenge → AuthProof → vérification à temps
  constant (compteurs et limites de tentatives inchangés) → TlsUpgrade →
  poignée de main → Auth/SessionState/ServiceList/Chat **chiffrés**. L'upgrade
  précède l'attente d'approbation : même l'attente est couverte.
- **Refus de démarrer sans runtime** : `HostConfig.tls_required` (défaut true) ⇒
  `start()` échoue `Unsupported` avec la raison exacte du runtime, aucun port
  ouvert, erreur affichée par l'interface. Le clair n'existe que si les DEUX
  côtés posent `tls_required=false`, et il est alors **étiqueté** :
  `HostSnapshot.transport`, `ParticipantRecord.transport`, `GuestSnapshot.transport`,
  `GuestPeer.transport`, `SessionInfo.transport` (bloc 1.3 optionnel du codec,
  absent en 1.2), pastilles UI (`status_success` pour TLS réel, `status_warning`
  pour clair assumé) — plus aucun « TCP CLAIR - TLS NON INTEGRE » codé en dur.
- **Suppression propre** : `secure_channel.{hpp,cpp}` retirés du dépôt,
  `MIGRATION_SOURCES` supprimé de `build.sh` et `build.bat` (compilation, objets,
  liens, liste `HEADERS`), cas de test de migration retiré. Le transport
  versionné passe de « tests seulement » à `NET_SOURCES` (production).
- **Cinq défauts réels découverts en branchant** (pas des régressions de tests) :
  trames avant admission inencodables (`validate_header`) ; identité PSK de 41 o
  servant d'identifiant de session OpenSSL (borne 32 o ⇒ « ssl session id too
  long », poignée de main morte des deux côtés — corrigé par SHA-256 de
  l'identité) ; **SIGPIPE** tuant le processus sur émission TLS vers un pair fermé
  (le BIO socket d'OpenSSL n'utilise pas `MSG_NOSIGNAL`) ; connexion morte retirée
  sans republication ⇒ pair fantôme à l'écran (`prune_dead_connections`) ; lecture
  unique par tour de poll côté invité ⇒ enregistrements TLS déjà déchiffrés
  laissés en attente (`SessionState`/`ServiceList` non vus) ; délai de poignée de
  main annoncé comme « erreur OpenSSL inconnue ».
- **Preuves exécutées** (`docs/reports/evidence/2026-11-18-etape5-basculement-tls13.txt`) :
  `unit: 38 cas, 21119 vérifications, 0 échec` (dont `hmac_sha256_matches_rfc4231`
  et `challenge_response_codec_and_derivation`) ; `platform: 20 cas, 760
  vérifications, 0 échec` ; `ipv6: 2 cas, 41 vérifications` ; `TEST SUMMARY
  passed=15 skipped=0` ; `TEST_EXIT=0`. Nouveaux cas :
  `control_plane_challenge_response_gates_admission` (protocole parlé à la main :
  clair refusé, défi réel de 32 o, preuve forgée refusée et comptée, preuve exacte
  ⇒ notice puis plus aucune trame claire lisible, et **le client brut qui ne fait
  pas la poignée de main n'est jamais admis** — capturer la notice ne donne rien) ;
  `lan_control_wire_never_carries_secrets` (relais octet-par-octet : 2558 octets
  observés, ni code texte ni code binaire, ni secret de reprise, ni messages, ni
  libellés de dérivation) ; `host_refuses_session_without_tls_runtime`.
  Auto-test renforcé : `PASS tls-host-refusal-proof` exige dans la même capture
  `[PASS] host_refuses_session_without_tls_runtime`, `host-refusal-proof:` et
  `host-cleared-proof: …clair` ; `platform: 16 cas, 693 vérifications, 0 échec`
  avec `DEVLINK_TLS_LIBRARY_PATH` introuvable, `SKIP tls13` (SKIP ≠ PASS),
  `SELFTEST_EXIT=0`. Capture de fil réelle : Hello/Challenge/Proof/Notice en
  `50 4c 50 54` (« PLPT ») puis enregistrements `16`/`14`/`17` avec suite `13 01`.
- **Limites assumées** : pas de confidentialité persistante (PSK dérivé d'un
  secret à durée de vie longue) ; code dictable à 80 bits protégé contre le
  cassage hors ligne par HMAC+nonce et contre la devinette en ligne par les
  limites de tentatives existantes — un PAKE (SPAKE2/OPAQUE) resterait plus
  robuste, à instruire ; fenêtre claire initiale sans secret (un attaquant actif
  peut couper, pas lire ni se substituer) ; Windows toujours
  IMPLEMENTED_BUT_NOT_VERIFIED (fabrique TLS indisponible ⇒ refus honnête).

## 14. Étape 8 — publications atomiques vers `releases/`, SHA-256 publié, jamais d'écrasement muet, 18 novembre 2026

- **Non-conformité traitée** : aucun répertoire `releases/` à la racine ; les
  archives restaient sous `out/packages/`, écrasées sans garde et non
  vérifiables par un tiers (§8 du plan).
- **Mécanique commune** (`publish_release` dans `build.sh`, `:publish_release`
  dans `build.bat`) : écriture `<archive>.partial`, intégrité lue de bout en
  bout AVANT publication (`tar -tzf` / `unzip -tq` / `tar.exe -tf`), SHA-256
  calculée puis publiée dans `<archive>.sha256` au format `sha256sum -c`
  (Windows : certutil + recontrôle final après renommage), renommage atomique,
  cible refusée hors de `releases/`. Une archive déjà publiée dont l'empreinte
  diffère provoque un échec EXPLICITE avec les deux empreintes — le pire défaut
  possible (remplacer une archive publiée) est rendu impossible silencieusement.
- **`package` vérifie ce qui sera reçu** : extraction de preuve, comparaisons
  octet par octet et exécution smoke portent désormais sur l'archive PUBLIÉE
  dans `releases/`, pas sur l'étape intermédiaire.
- **`clean` ne touche jamais `releases/`** : la branche n'énumère que
  `out/{debug,release,tests,packages,doctor}` ; démontré par une preuve NON
  VIDE (quelque chose doit réellement disparaître sous `out/`, sinon la
  préservation serait prouvée par rien) puis survie des artefacts publiés.
- **Auto-test** : `PASS release-publish-proof` (publication réelle d'une fixture,
  empreinte revérifiée, absence de `.partial`) + deux contrôles négatifs exigés
  (contenu différent sous le même nom refusé avec original intact ; archive
  illisible jamais acceptée, sans résidu) ; `PASS clean-preserves-releases-proof`.
  Sorties réelles : `docs/reports/evidence/2026-11-18-etape8-packaging-releases.txt`.
- **Dépôt propre** : `releases/.gitignore` ignore `*.tar.gz`, `*.zip`, `*.partial` ;
  seules les empreintes `*.sha256` et le mode d'emploi sont versionnés.
- **Limite assumée** : le côté Windows (`:publish_release`, tar.exe/certutil/
  move) suit exactement la procédure prouvée côté bash mais reste
  IMPLEMENTED_BUT_NOT_VERIFIED depuis Linux (même convention qu'ADR-016).
