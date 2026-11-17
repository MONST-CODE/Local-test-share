# DevLink — règles de marque

Ce document est la référence d'utilisation des ressources de `assets/branding/`.
Il décrit ce que chaque fichier EST, d'où il vient, et comment le régénérer.
Toute ressource qui ne peut pas être rattachée à une règle de ce document ne
doit pas être ajoutée au dépôt.

## 1. Le signe `<-D->`

Interprétation officielle (mandat §14) :

- `<` et `>` sont des **chevrons de code** (délimiteurs d'inclusion de
  bibliothèque), ouverts, à angles arrondis : ce ne sont **pas** des pointes de
  flèche, aucun triangle n'est admis ;
- deux **petits tirets horizontaux** séparent les chevrons du D ;
- le **D** est la forme centrale et dominante (capitale 92 u sur une grille de
  240 u, hampe + panse en cubiques de Bézier) ;
- symétrie exacte autour de `x = 120` : chevron gauche 25↔215 chevron droit,
  tirets 73↔167, D 94↔146 ;
- aucun effet 3D, aucun dégradé, aucune ombre, **aucun slogan dans l'icône
  d'application** ; le mot `DevLink` est un wordmark séparé, jamais intégré au
  symbole.

Grille de référence : 240 × 240, marge optique ≥ 9 u. Épaisseurs : chevrons
15 u, tirets 13 u, D 16 u (variante standard) ; chevrons 22 u, D 26 u sans
tirets (variante compacte, petites tailles).

`DevLink-Brand-Reference.png` (racine du mandat) n'est qu'une **direction
artistique** : elle n'a jamais servi de source bitmap, n'est pas embarquée dans
les binaires, et aucun tracé n'en est issu par calque.

## 2. Fichiers et usages

| Fichier | Usage | Notes |
|---|---|---|
| `devlink-icon-master.svg` | source de vérité géométrique | contient `#dl-standard`, `#dl-compact` et leurs variantes tuilées `#dl-*-tile` |
| `devlink-symbol.svg` | symbole seul, fonds maîtrisés (UI sombre, documents) | couleurs = rôles DevDark |
| `devlink-logo.svg` | verrouillage horizontal symbole + wordmark (40 %) | espace symbole→wordmark 44 u, ne pas resserrer |
| `devlink-wordmark.svg` | le mot « DevLink » seul | tracés vectoriels géométriques, aucune police |
| `devlink-mono-dark.svg` | monochrome clair `#F5E9DC` pour fonds sombres | gravure, impression 1 couleur |
| `devlink-mono-light.svg` | monochrome foncé `#453329` pour fonds clairs | idem fonds clairs |
| `devlink-16.png` … `devlink-256.png` | exports raster versionnés | 16/24 = variante compacte tuilée ; ≥ 32 = standard tuilée |
| `devlink.ico` | icône Windows multi-tailles | 7 entrées réelles : 16/24/32/48/64/128 en DIB 32 bpp + 256 en PNG |
| `app/desktop_ui/design_system/window_icon_data.{hpp,cpp}` | RGBA 64 × 64 embarqué pour `SDL_SetWindowIcon` | généré depuis le maître ; repli quand aucune icône PE n'existe (Linux) |

La **tuile de fond** arrondie **cuivre `#A47A59`** (rôle accent DevLight,
teinte de la planche de direction artistique) portant la marque **monochrome
crème** n'existe que dans les exports d'icône d'application (`*-tile`) : le fond
d'un bureau ou d'un lanceur est inconnu, la tuile garantit le contraste, et le
monochrome crème sur cuivre reste lisible à 16 px. Elle est **interdite** dans
l'interface, où le symbole bicolore vit sur les surfaces du thème.

## 3. Couleurs

Rôles officiels (design system, `design_system/themes.cpp`) :

- DevDark : accent clair `#C49A78` (chevrons, tirets), texte principal
  `#F5E9DC` (D) ; icône d'application : tuile cuivre `#A47A59` + marque
  monochrome `#F5E9DC` ;
- DevLight : accent `#A47A59`, texte principal `#453329`.

Le signe couleur n'est utilisé que sur fond maîtrisé ; ailleurs, monochrome.
Contraste minimal visé pour le signe : 4.5:1 entre chaque rôle et son fond.

## 4. Tailles minimales et variante compacte

- ≥ 32 px : variante **standard** (chevrons + tirets + D) ;
- 16 et 24 px : variante **compacte** (chevrons épaissis, tirets retirés, D
  épaissi) : à ces tailles les tirets deviendraient du bruit ;
- en dessous de 16 px : ne pas utiliser le signe, utiliser le wordmark ou le
  nom texte.

## 5. Interdits

- déformer (étirement non uniforme), incliner, ajouter contour/ombre/dégradé ;
- recomposer les éléments (déplacer les tirets, changer leur nombre) ;
- remplacer le wordmark par une police système ;
- poser le signe couleur sur un fond non vérifié (utiliser les monochromes) ;
- intégrer un slogan, une baseline ou une mention dans l'icône d'application ;
- renommer un PNG en `.ico` : le ICO du dépôt est produit et **vérifié**
  (en-tête, répertoire, DIB/PNG, tailles requises) par la sonde de build
  `probe --ico`, et par `export_icons.py --verify`.

## 6. Régénérer les exports

```bash
python3 tools/branding/export_icons.py --verify   # écrit PNG + ICO + RGBA embarqué puis relit tout
python3 tools/branding/export_icons.py --check    # vérifie les fichiers déjà versionnés
```

L'outil est un **outil de maintenance**, jamais appelé par `build.sh` ni
`build.bat` : le build n'a pas besoin de Python. Il lit le sous-ensemble SVG
documenté (M/L/C/Z, translate/scale, tracés arrondis) et échoue explicitement
sur toute construction non prise en charge. Les artefacts binaires produits
sont versionnés ; le build et le runtime ne consomment qu'eux.

Le build Windows vérifie en plus que le binaire applicatif porte réellement la
ressource : `probe --pe-icons devlink-desktop.exe --expect-icons 7` lit les
entrées `RT_GROUP_ICON`/`RT_ICON` du PE (pas le fichier `.ico`).

## 7. Licence

Géométrie et tracés : production originale DevLink, mêmes termes que le dépôt.
Aucune police tierce n'est embarquée à ce jour : la typographie de
l'interface charge une famille OFL si le dépôt en fournit une
(`assets/fonts/`, avec son `NOTICE.md`), sinon une famille système du contexte
(Segoe UI / DejaVu Sans / Noto Sans / Liberation Sans), sinon un repli ImGui
**signalé** dans l'écran de diagnostics — jamais un succès prétendu.
