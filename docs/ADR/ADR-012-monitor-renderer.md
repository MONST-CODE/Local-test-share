# ADR-012 — SDLRenderer3 pour le moniteur, pas pour figer le pipeline vidéo

Date : 2026-11-13. Statut : **provisoire, périmètre moniteur uniquement**.

## Contexte et options

ADR-001 valide SDL3/ImGui. Le maître laisse SDL_GPU/Vulkan à comparer. Le moniteur
n'a ni frame vidéo distante ni surface WebView2 à décoder. Choix envisagés :
SDL_GPU, Vulkan/direct backend plateforme, ou backend officiel ImGui SDLRenderer3.

## Décision

Utiliser le backend officiel SDLRenderer3 pour ce premier écran de découverte :
faible complexité, build Windows/Linux, driver software/dummy disponible pour tester
sans bureau. SDL3 3.4.20 et ImGui 1.93.0 sont épinglés par commit et SHA-256.
L'inventaire expose licences, provenance, risques et mise à jour. Pas d'assets Web,
CDN, télémétrie ni serveur HTTP pour l'UI.

La configuration active l'UI seulement sur demande. Audio, joystick, haptic, sensor,
camera, HIDAPI/libusb et l’API SDL_GPU sont désactivés au build. Le renderer reste une dépendance de fenêtre
et de drivers système, pas une frontière de sandboxing.

## Conséquences

Ce choix ne valide **pas** le renderer final du flux distant. SDL_GPU, D3D/Vulkan,
partage de surfaces, encodage matériel et coûts de copies doivent être évalués au
Lot E. Pas d'affirmation 30 FPS / 720p à partir d'un smoke test de dashboard.

L'UI de découverte conserve ses workers et états indépendamment d'un futur renderer.
Le build `PLPT_HEADLESS_UI=ON` ne contient pas de backend desktop Linux complet ;
il sert uniquement aux tests du sandbox. Le support multi-écran/DPI/IME natif
Windows doit être testé avant fermeture du Lot D.
