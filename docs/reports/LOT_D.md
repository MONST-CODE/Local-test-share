# Lot D — registre et moniteur SDL3 / Dear ImGui

Date : 2026-11-13. **UI réelle et tests headless Linux ; interaction/affichage Windows non qualifiés.**

## Objectif et fichiers

`engine/discovery/{process_tree,terminal_classifier,monitor}.*`,
`engine/service_registry/registry.*`, `app/{host_shell,desktop_ui}/main.cpp`,
`cmake/Desktop.cmake`, `tests/{unit,cli_contract.cmake,desktop_errors.cmake}`,
`third_party/DEPENDENCIES.md` et les licences principales/notices embarquées.

Inventaire, filtre, inspection, états et sélection locale. Commandes worker bornées,
vues immuables, conservation des warnings et invalidation des sélections sur toute
observation incomplète. **Aucun état de sélection n'ouvre un partage.**

## Décisions / dépendances

ADR-001/005 et [ADR-012](../ADR/ADR-012-monitor-renderer.md). SDL3 3.4.20, ImGui 1.93.0,
commits et hashes épinglés ; licences zlib/MIT. Renderer officiel SDLRenderer3 pour
le moniteur uniquement, pas une décision sur le futur décodage vidéo.

## Résultats et corrections

- Build Linux réel SDL/ImGui ; **7/7 entrées CTest** sur le profil dummy/software,
  et **7/7** sur le profil ASan+UBSan après correction du chemin d'erreur BMP.
- Smoke : 8 frames, snapshot 1, 8 groupes observés. Capture BMP puis conversion PNG ;
  [capture réelle](evidence/2026-11-13/desktop.png), pas une maquette générée.
- Capture revue visuellement : navigation, contenu, table et inspection rendus ;
  padding corrigé, libellé latéral non tronqué. La table a un défilement horizontal
  volontaire, ses huit colonnes ne sont pas toutes visibles simultanément à 1400 px.
- Test screenshot vers chemin invalide : **échec initial LeakSanitizer**, 3 780 400
  octets dans le chemin SDL_SaveBMP ; log conservé. Appel remplacé par IO explicite,
  propriété RAII et fermeture vérifiée. Re-tests ouverture impossible, disque plein
  et capture réussie sans diagnostic. Bibliothèque SDL non modifiée.
- Tests worker : queue 32, refus du 33e, snapshot immuable, sélection supprimée après
  erreur/incomplétude, pas de restauration silencieuse ; 25 répétitions ASan réussies.

Commandes exactes et limites des outils : [VALIDATION.md](VALIDATION.md).

## Non exécuté / risques résiduels

Aucun clic ou parcours clavier natif Windows n'a été exercé. Pas de qualification
DPI multi-écran, IME, D3D/GPU, CPU en arrière-plan ni d'installateur Windows. Un
smoke `dummy` ne prouve ni l'interactivité complète ni le rendu d'une page distante.
Le module de projet reste non attribué plutôt que déduire des secrets de `.env`.

## Prochaine porte

Qualification Windows B/C/D et prototype isolé WebView2/capture E (risque critique).
Ne pas ajouter davantage de fonctionnalités sociales avant cette preuve de faisabilité.
