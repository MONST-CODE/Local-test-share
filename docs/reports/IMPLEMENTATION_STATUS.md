# État d'implémentation vérifié — DevLink 0.2.0-preview

**Date :** 2026-11-21. **Base :** branche `arena/125b0385-local-test-share`, commit
`8351a12` + modifications de cette session (non commitées à la rédaction).
**Environnement d'exécution :** Windows 11 Pro x64 (10.0.26200), MSYS2 UCRT64,
clang 22.1.8 (`x86_64-w64-windows-gnu`), SDL3 3.4.20-1, Dear ImGui `c1aaac6`,
runtime WebView2 présent. **Une seule machine.**

Ce document **remplace comme référence d'état** `FINAL_COMPLETION_AUDIT.md`
(rédigé avant le Browser Worker WebView2, il le déclare absent) et
`CURRENT_STATUS.md` (branche antérieure). Ces rapports restent des archives
datées. Vocabulaire : **VÉRIFIÉ** = exécuté ici, résultat conforme ;
**IMPLÉMENTÉ** = code et tests présents, preuve physique manquante ;
**PARTIEL** ; **ABSENT**. Un SKIP (code 77) n'est jamais compté comme réussi.

## 1. Exécutions de cette session

| Commande | Résultat |
|---|---|
| `build.bat doctor` | VÉRIFIÉ — 3/3 étapes ; ASan et libFuzzer UCRT64 indisponibles (SKIP signalé, jamais PASS) |
| `build.bat test debug` (état initial, avant corrections) | **ÉCHEC** — `desktop-e2e` : capture de la fenêtre invitée vide (0 pixel vert). Les 5 autres suites passaient. Cause et correction : §4 |
| `build.bat test debug` (après corrections) | VÉRIFIÉ — 11/11 étapes, 17 tests réussis, 0 ignoré, 6 min 00 s : unit 42 cas / 21 348 vérifications ; platform 16 / 537 ; ipv6 2 / 41 ; browser 5 / 116 ; share 6 / 137 ; desktop-e2e 1 / 13 (27 224 pixels verts dans la vraie fenêtre invitée) ; 5 contrôles CLI ; 6 contrôles UI |
| `build.bat test release --native-ui` | VÉRIFIÉ — 11/11 étapes, 18 tests réussis (dont la vraie fenêtre Windows et `desktop-e2e`), 6 min 13 s |
| `build.bat selftest` | VÉRIFIÉ — 7/7 garde-fous (MSYS2 absente, CLANG64, compilateur imposé, échecs de compilation et de liaison sans reliquat, archive corrompue, code 23) |
| `build.bat ide-check` | VÉRIFIÉ — 4/4, contrat des tâches VS Code |
| `build.bat fuzz` | SKIP (code 77) — libFuzzer UCRT64 non opérationnel sur ce poste ; jamais compté comme réussi |
| `build.bat test debug --native-ui` (après correction du test e2e) | voir §6 |
| `build.bat package` | **NON EXÉCUTÉ** volontairement : il publie une archive dans `releases/`, ce qui exige une décision du propriétaire et une qualification LAN préalable (REL-01) |
| `build.sh` (Linux) | **NON EXÉCUTÉ** sur Linux natif (aucun Linux de développement ici). Syntaxe (`bash -n`), aide et mécanique d'affichage vérifiées sous Git Bash |

## 2. Exigences fonctionnelles du maître (§5)

### Découverte (§5.1)

| ID | Statut | Preuve / réserve |
|---|---|---|
| FR-DIS-001/002/010 TCP v4/v6, UDP distincts, pas de confusion | VÉRIFIÉ | `platform` (sockets réels, port 0), moniteur |
| FR-DIS-003/004/005 PID, processus, arbre avec données manquantes | VÉRIFIÉ | `unit` + `platform` |
| FR-DIS-006/007 terminal/IDE probable et confiance | IMPLÉMENTÉ | `terminal_classifier` ; pas de campagne Next.js/Vite/Flask réelle |
| FR-DIS-008 inventaire sans scan agressif | IMPLÉMENTÉ | rafraîchissement périodique demandé par l'UI |
| FR-DIS-009 Docker en lecture seule, opt-in | **PARTIEL** | seul le nom de processus (`docker-proxy`, `com.docker.backend`) est classé ; pas d'adaptateur Docker |

### Sélection et autorisation (§5.2)

| ID | Statut | Preuve / réserve |
|---|---|---|
| FR-SHR-001/002 choix explicite, rien de partagé par défaut | VÉRIFIÉ | « Choisir » ne partage rien ; allowlist vide = rien |
| FR-SHR-003 origines confirmées par l'hôte | VÉRIFIÉ | allowlist exacte (`url_origin.hpp`) + garde SOCKS5 : 0 connexion au port canari (`browser`) |
| FR-SHR-004 bases, consoles, sockets internes non exposés | VÉRIFIÉ | aucune origine implicite ; `localhost` non autorisé d'office |
| FR-SHR-005/006 révocation individuelle, arrêt de session | VÉRIFIÉ | `share`, `platform` |
| FR-SHR-007 expiration configurable | **PARTIEL** | `Session::is_expired` au modèle ; pas de réglage dans l'UI |

### Multi-invités (§5.3)

| ID | Statut | Preuve / réserve |
|---|---|---|
| FR-MUL-001 plusieurs invités | VÉRIFIÉ (boucle locale) | `share` : 5 invités ; jamais 5 machines |
| FR-MUL-002/007 profil isolé par testeur (cookies, stockage) | VÉRIFIÉ | mode A : profils InPrivate distincts, cookies/localStorage séparés (`share`) ; le backend applicatif reste commun (affiché) |
| FR-MUL-003 vue collaborative, verrou unique | VÉRIFIÉ | mode B, bail à époque, transfert (`share`, `desktop-e2e`) |
| FR-MUL-004 observation seule | VÉRIFIÉ | mode C : entrées valides d'un client modifié refusées (`share`) |
| FR-MUL-005 états connecté/inactif/reconnexion/déconnecté | IMPLÉMENTÉ | états publiés ; reconnexion automatique désactivée par conception (consentement) |
| FR-MUL-006 capacité limitée | VÉRIFIÉ | `max_participants`, refus au-delà |

### Messagerie et feedback (§5.4)

| ID | Statut | Preuve / réserve |
|---|---|---|
| FR-MSG-001 discussion générale | VÉRIFIÉ | `platform` |
| FR-MSG-002 messages privés | VÉRIFIÉ | invité↔invité, **invité→hôte et hôte→invité ajoutés cette session** (ADR-019) ; UI : sélecteur de destinataire |
| FR-MSG-003 présence, saisie, accusés de lecture | **PARTIEL** | états de présence des pairs publiés ; ni indicateur de saisie ni accusé de lecture réseau (`mark_read` au modèle seulement) |
| FR-MSG-004 réponses, réactions, mentions, épinglage | **ABSENT** au réseau | `Messaging::reply` au modèle seulement |
| FR-MSG-005 signalement contextualisé (P0) | VÉRIFIÉ | **ajouté cette session** : protocole 1.5, registre hôte, accusé à l'auteur seul, UI des deux côtés (ADR-019) |
| FR-MSG-006 capture jointe sur action explicite (P1) | ABSENT | refusée explicitement par le modèle |
| FR-MSG-007 pas de conservation après fermeture | IMPLÉMENTÉ | mémoire seulement, bornée ; pas d'audit mémoire/disque |
| FR-MSG-008 pas de pièce jointe arbitraire | VÉRIFIÉ | aucun type de message ne la porte |

### Rendu distant (§5.5)

| ID | Statut | Preuve / réserve |
|---|---|---|
| FR-RND-001 navigateur sur l'hôte | VÉRIFIÉ | WebView2 hors écran (`browser`) |
| FR-RND-002 images sans ressources Web chez l'invité | VÉRIFIÉ | JPEG du compositeur sur canal TLS 1.3 par invité ; le proxy HTTP ne fait plus partie du partage |
| FR-RND-003 souris/clavier ordonnés et validés | VÉRIFIÉ | séquence strictement croissante, rejeu refusé, `SharePolicy` côté hôte |
| FR-RND-004 redimensionnement, DPI, navigation, WebSockets | **PARTIEL** | viewport fixe 1280×720 (letterbox côté invité, clics justes quel que soit le DPI) ; pas de redimensionnement dynamique |
| FR-RND-005 téléchargements, presse-papiers, fichiers, protocoles externes interdits | VÉRIFIÉ | `browser` : `file:`, `javascript:`, `ms-settings:`, fenêtres et téléchargement bloqués ; presse-papiers : aucun chemin de code ne le relaie (non testé comme tel) |
| FR-RND-006 aucune navigation hors origines | VÉRIFIÉ | navigations, redirections, sous-ressources, fenêtres (`browser`) |
| FR-RND-007 contexte détruit en fin de session | VÉRIFIÉ | `contexts=N/N` à l'arrêt (`desktop-e2e`) |

## 3. Critères d'acceptation V1 (§23)

Atteints sur une machine : 1, 2, 3 (limites montrées), 4, 5, 6, 10, 11 (backend
partagé signalé), 12, 13, 14, 15, 16, 17 (selon protocole de nettoyage), 18.
**Non atteints** : 7 et 8 sur un vrai LAN (découverte multicast propriétaire, pas
mDNS ; jamais exécutée entre deux machines) ; 9 (cinq testeurs sur la machine de
référence : non mesuré) ; 19 (performances mesurées sur une machine seulement,
ADR-018 §3) ; 20 (le rapport LAN de `REAL_LAN_QUALIFICATION.md` reste à produire).

## 4. Défauts trouvés et corrigés dans cette session

1. **`desktop-e2e` fragile sur un poste en cours d'utilisation** (en échec sur
   l'état commité). Le test ouvre de vraies fenêtres actives et place le vrai
   curseur sur le bouton de la page : une minimisation ou un clic réel pendant le
   test faussait la preuve. Deux symptômes observés ici : capture réduite à la
   couleur de fond (fenêtre minimisée : le backend ImGui met `DisplaySize` à 0 et
   ne dessine rien) ; carré rouge après **44 clics** reçus au lieu d'un (la couleur
   vaut `clics mod 4`). Corrections : une trame sans surface d'affichage n'est
   jamais enregistrée (fenêtre restaurée, capture reportée, état consigné) ; la
   couleur capturée doit correspondre à un état **réellement atteint par l'hôte**
   (clics relevés dans son DOM) ; toute entrée extérieure est consignée en preuve.
   Il reste un test de poste réel : ne pas utiliser la souris pendant son exécution.
2. **Course de données** dans `GuestClient::post_direct_message` (lecture de
   `state_.self`, propriété du thread d'E/S, depuis le thread UI) : lecture de
   l'instantané publié sous mutex.
3. **Signalements non reliés au réseau** (P0) et **messages privés vers l'hôte
   impossibles** : protocole 1.5 (ADR-019), écrans réels.
4. **Test de version codé en dur** (`negotiate_version(1, 5)` supposé refusé, et
   commentaire « 1.4 n'existe pas » faux) : dérivé de `kProtocolMinor`.
5. **`PLPT_IMGUI_DIR` documenté et lu par VS Code, ignoré par `build.bat`** :
   accepté comme ancien nom de `DEVLINK_IMGUI_DIR`.
6. **Sortie de build illisible** : 848 460 lignes pour un `test debug`
   (pacman, certutil, objdump…). Nouvelle interface : §5.
7. Documentation contradictoire avec le code (WebView2 « absent », TLS Windows
   « à câbler », protocole « 1.3 », 502 d'un proxy hors partage…) : corrigée.

## 5. Interface de build

`build.bat` et `build.sh` affichent le bandeau DEVLINK (UTF-8, dégradé marron de
la marque), une ligne par phase réelle `[n/N]` avec progression réécrite sur
place, des lignes de résultat par suite de tests et un résumé. Aucune sortie
d'outil n'est affichée hors échec ou `--verbose` ; tout est journalisé. Un
`test debug` passe de 848 460 à environ 40 lignes. Détails : `docs/BUILD.md`,
« Lire la sortie du build ».

## 6. Batterie finale

Un passage de `test debug --native-ui` a échoué sur `desktop-e2e` pendant que le
poste était utilisé (44 clics réels reçus par la fenêtre invitée, carré rouge au
lieu de vert) : c'est ce qui a permis d'établir la cause décrite en §4.1. Après
correction du test, résultat du passage final : **VÉRIFIÉ** — 11/11 étapes, 18 tests réussis,
0 ignoré, 5 min 11 s : unit 42 cas / 21 348 vérifications ; platform 16 / 537 ;
ipv6 2 / 41 ; browser 5 / 116 ; share 6 / 137 ; desktop-e2e 1 / 16 (preuve :
carré vert, 27 225 pixels, 1 clic côté hôte, fenêtre active, 0 trame reportée) ;
5 contrôles CLI ; 7 contrôles UI dont la vraie fenêtre Windows. Rendu de la
console (simulation fidèle des séquences VT émises) :
`evidence/2026-11-21-build-console-test-debug-native.png`.

## 7. Reste à faire, par priorité

1. **Qualification LAN physique** (deux PC Windows) selon
   `REAL_LAN_QUALIFICATION.md`, modes A/B/C, révocation, pare-feu.
2. Mesures sur machine de référence : cinq invités, FPS, latence p95, mémoire.
3. Messagerie P1 : présence/saisie/accusés de lecture, réponses, réactions,
   mentions, épinglage ; capture jointe sur consentement (FR-MSG-006).
4. Redimensionnement dynamique du viewport et DPI de l'invité (FR-RND-004).
5. Expiration de session réglable (FR-SHR-007) ; adaptateur Docker opt-in
   (FR-DIS-009) ; mDNS/DNS-SD (maître §10.5).
6. Invité Linux (décodeur JPEG portable épinglé) ; `build.sh` exécuté sur Linux
   natif et en CI avec la nouvelle interface.
7. Release candidate (`package`) seulement après 1 et 2.
