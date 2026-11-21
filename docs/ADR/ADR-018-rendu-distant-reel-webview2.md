# ADR-018 — Rendu distant réel : Browser Worker WebView2, canal média TLS 1.3 par invité, autorité des entrées côté hôte

- **Statut :** accepté, **implémenté** ; qualifié en boucle locale sur Windows x64
  (une machine). La qualification LAN physique (deux machines) reste **à exécuter**.
- **Date :** 2026-11-21
- **Remplace :** la ligne « À instruire : Browser Worker / capture de surface » de
  l'index des ADR. Complète ADR-013 (proxy vs rendu distant), ADR-016 et ADR-017
  (transport TLS 1.3 PSK, défi-réponse).

## Contexte

Le cœur de DevLink est le rendu exécuté sur l'hôte : un vrai navigateur charge
l'application autorisée, ses pixels partent chez l'invité, les entrées de l'invité
reviennent et ne sont injectées qu'après une autorisation **côté hôte**. Jusqu'au
commit `377594b`, `WebView2Backend` échouait fermé, le codec média n'était relié à
rien, et le poste desktop publiait le service local par un proxy HTTP. Ce proxy
contredit le principe : il transmet aux invités le HTML, le JavaScript et les
ressources du projet.

## Décisions

### 1. Browser Worker : WebView2 hors écran, un profil par contexte

- Un thread STA dédié possède l'environnement WebView2. Chaque contexte a sa fenêtre
  hors écran et son profil **InPrivate**, avec des cookies, un cache, un
  `localStorage` et un `sessionStorage` indépendants (mode A).
- **Pixels :** le flux du compositeur Chromium (`Page.startScreencast` via
  `CallDevToolsProtocolMethod`) fournit des JPEG du rendu réel. L'accusé de
  réception de chaque image sert de contre-pression. Un emplacement « dernière
  image » par contexte (`wait_frame`) évite toute file d'images périmées.
- **Entrées :** `Input.dispatchMouseEvent`, `Input.dispatchKeyEvent` et `Input.insertText`
  produisent des événements `isTrusted` (la fixture refuse les autres).
- **Alternatives écartées pour la V1 (non mesurées ici) :** `CapturePreview`
  produit une image à la demande, pas un flux poussé avec contre-pression.
  Windows.Graphics.Capture capture une fenêtre composée à l'écran, pas un contexte
  hors écran par invité. Changer de moteur (CEF, Chromium piloté) imposerait de
  distribuer un runtime navigateur, alors que WebView2 est un composant système de
  Windows 10/11 et que la chaîne mesurée atteint la cible de latence. Toute remise
  en cause passera par une nouvelle ADR, avec un prototype et des mesures.

### 2. Politique d'origines et garde réseau

L'allowlist s'applique aux navigations, redirections, sous-cadres,
sous-ressources, fenêtres, téléchargements, permissions et schémas externes
(`file:`, `javascript:`, `data:` interdits). Le filtrage des requêtes ne suffit
pas : un test hostile a mesuré 3 préconnexions TCP de Chromium vers un port
interdit. Le navigateur ne sort donc que par **EgressGuard**, un proxy SOCKS5
DevLink (boucle locale incluse) qui n'ouvre que les couples `hôte:port` autorisés.
Mesure après correction : 0 connexion au port canari. Aucun service `localhost`
n'est autorisé implicitement.

### 3. Codec : JPEG du compositeur, décodage WIC

- Le JPEG est produit par le compositeur, sans encodeur à ajouter. Il est validé
  contre les dimensions SOF avant décodage. Le décodeur invité est WIC, composant
  de l'OS : aucune dépendance supplémentaire, aucune licence à gérer.
- **Mesures (une machine, fixture animée à environ 10 i/s, 1280×720) :** environ
  13 Ko par image, 1,1 à 1,6 Mbit/s par invité, décodage de 3 à 4 ms, clic
  → image modifiée en 19 à 42 ms, 5 invités ≥ 9,8 i/s (5,5 Mbit/s au total).
- **Limite assumée :** le débit d'images suit les changements de la page (le
  compositeur n'émet rien pour une page statique). H.264 matériel (Media Foundation)
  réduirait le débit d'un ordre de grandeur sur les pages très animées. C'est une
  optimisation, à mesurer avant d'en faire une décision. Aucun codec maison.

### 4. Canal média : une connexion TLS 1.3 PSK par invité, séparée du contrôle

- Après admission, et **seulement sur un canal de contrôle TLS**, l'hôte envoie un
  `MediaOffer` (protocole 1.4) : port média, identité PSK publique
  (`devlink-media-s<session>-p<participant>`) et clé aléatoire de 32 octets **propre à
  ce participant**. Il n'y a jamais d'offre en clair.
- L'invité ouvre un TCP, envoie un `Hello` en clair qui ne désigne que la
  concession, puis négocie TLS 1.3 PSK avec cette clé. Une mauvaise clé, une
  identité usurpée, une autre session, du bruit avant TLS ou un client TLS 1.2
  (downgrade) échouent à la poignée de main. Les échecs sont comptés et limités
  (8 poignées de main en cours au plus, 10 échecs par minute et par adresse).
- **Enregistrements :** longueur u32 big-endian vérifiée **avant** allocation, puis
  le type. L'hôte envoie `Frame`, `State` et `Closing` ; l'invité envoie `Input` et
  `Control`. Jamais plus d'un enregistrement en attente par invité : la latence ne
  s'accumule pas.
- Le canal est séparé du contrôle pour que les images ne retardent ni le chat ni les
  battements. Le média écoute sur **la même interface** que le contrôle, résolue
  une seule fois, sur un port choisi par l'OS et gardé ouvert (pas de TOCTOU).
- Un départ, une révocation ou une nouvelle admission invalide la clé : l'ancien
  canal est coupé et la clé ne rouvre plus rien.

### 5. Autorité des entrées : `SharePolicy`, côté hôte uniquement

Chaque enregistrement `Input` est réautorisé (participant connu, non révoqué,
séquence strictement croissante, partage non suspendu, mode, bail) avant toute
injection. Une séquence refusée est consommée : elle ne peut pas être rejouée
après un changement de bail.

| Mode | Contexte navigateur | Entrées |
|---|---|---|
| A — test indépendant | un profil par invité, créé hors du thread UI | chacun pilote **son** contexte |
| B — collaboration | un contexte partagé | un seul détenteur du bail (époque incrémentée), file FIFO, l'hôte peut donner ou reprendre le contrôle |
| C — observation | un contexte partagé | **toujours refusées**, même pour un client modifié |

Le mode A isole le **navigateur**, pas le backend de l'application (base de
données, API) : l'interface et la documentation le disent.

### 6. Cycle de vie transactionnel

`Validating → StartingBrowser → Navigating → AwaitingFirstFrame →
OpeningMediaChannel → Active`. L'état `Active` n'est publié qu'après une **vraie
première image** et l'ouverture du port média. Tout échec annule chaque étape
(navigation hors allowlist incluse). `stop()` est idempotent : canaux coupés,
contextes détruits, processus navigateur terminé, profil temporaire supprimé. Le
poste desktop lance ce démarrage dans un thread : l'UI ne bloque jamais (un
premier contexte WebView2 prend environ 5 s à froid).

### 7. Poste desktop : le proxy HTTP sort du chemin de partage

Le bouton « Démarrer le partage » lance `ShareHost` et `HostServer`. Il n'y a plus
de `ServiceProxy` dans ce parcours (le module et ses tests restent pour un usage
explicite futur, ADR-013). L'invité affiche les images dans une texture SDL3
(thread principal), en letterbox. Les coordonnées sont normalisées par rapport à
l'**image** : la taille du panneau, les bandes noires et le DPI ne décalent pas un
clic (`engine/share/view_mapping.hpp`, testé). Le clavier n'est capturé qu'après un
clic dans la vue, et un clic hors de la vue le libère. Le presse-papier n'est pas
relayé.

## Conséquences

- **Hôte et invité : Windows 10/11 x64** (WebView2 côté hôte, WIC côté invité).
  Un invité Linux exige un décodeur JPEG portable (libjpeg-turbo, à épingler) :
  c'est une étape séparée, sans prétention actuelle.
- Les rapports distinguent : tests moteur en boucle locale (`devlink-share-tests`),
  bout en bout sur les vrais exécutables (`devlink-desktop-e2e-tests` : service de
  démonstration + `devlink-desktop --host` + `devlink-desktop --join`, vraie fenêtre
  SDL3) et **LAN physique**, toujours bloqué tant que deux machines n'ont pas
  exécuté `docs/reports/REAL_LAN_QUALIFICATION.md`.

## Tests

`tests/unit` (politique A/B/C, enregistrements bornés, mappage letterbox, découpage
UTF-8), `tests/integration/browser_pipeline_tests.cpp`,
`share_pipeline_tests.cpp` (modes A/B/C, attaques, 5 invités, cycles) et
`desktop_e2e_tests.cpp`. Le code de sortie 77 vaut SKIP, jamais PASS.
