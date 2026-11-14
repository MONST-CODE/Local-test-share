# ADR-013 — Rôle du proxy HTTP et frontière de confiance avec le rendu distant

Date : 2026-11-14. Statut : décision d'implémentation de l'incrément « stabilisation »
(Phase A du maître). Ne vaut pas acceptation V1.

## Contexte

Le produit promis est un **rendu distant** : un Browser Worker sur la machine de
l'hôte navigue dans l'application, sa surface est capturée, encodée, transportée sur
un canal sécurisé, décodée puis rendue dans l'interface SDL3 de l'invité ; les
entrées de l'invité repartent vers le navigateur de l'hôte.

État réel du dépôt au moment de cette décision :

- ce chemin est **absent**. `engine/browser/browser.hpp` ne contient que
  `IRemoteBrowserBackend` et `NullBrowserBackend`, qui échoue explicitement. Aucune
  capture, aucun encodeur, aucun décodeur, aucun backend WebView2 ;
- ce qui existe réellement est un plan de contrôle (`net/host_server`,
  `net/guest_client`, `net/discovery`, messagerie) et un plan de données
  (`net/proxy`) : pont TCP autorisé par un jeton de capacité, HTTP/1.1 et WebSocket
  transparents après autorisation.

Le risque nommé par le maître est précisément celui-là : un proxy HTTP fonctionne,
il donne l'illusion du produit, et il peut être présenté à tort comme du « partage
d'application ». Deux défauts concrets étaient présents dans le code :

1. **fuite de capacité vers l'amont** : l'en-tête de requête était transmis tel quel,
   jeton compris (`?plpt=<jeton>`, `Cookie: plpt=…`, `X-PLPT-Token`). Le secret de
   capacité PLPT se retrouvait donc dans les journaux du service du développeur,
   dans son historique, et dans les `Referer` des requêtes suivantes ;
2. **sous-ressources refusées** : seul le document initial portait le jeton. Une page
   réelle (CSS, JS, images, XHR) recevait `403` sur toutes ses sous-ressources : le
   proxy ne pouvait pas servir une application web complète, seulement son premier
   document.

## Options

1. Supprimer le proxy et attendre le Browser Worker.
2. Garder le proxy tel quel et le présenter comme « partage d'application web ».
3. Garder le proxy dans un rôle **subordonné et explicite**, corriger la fuite de
   capacité, rendre les sous-ressources autorisables sans répéter le jeton, et écrire
   la frontière de confiance.
4. Remplacer le proxy par un mandataire TLS terminant sur l'hôte (interception du
   service local).

## Décision

**Option 3.**

### Rôles

Le chemin principal du produit reste : détection native → confirmation explicite de
l'hôte → Browser Worker → capture de surface → encodage → transport sécurisé →
décodage → rendu SDL3 → entrées renvoyées à l'hôte. **Le proxy n'est pas ce chemin**
et ne doit jamais être présenté comme tel, ni dans l'interface, ni dans les rapports.

Rôles admis du proxy, tous subordonnés à une confirmation explicite de l'hôte pour
chaque service :

- consultation d'un service web local par des testeurs autorisés, quand l'hôte le
  choisit délibérément (mode « service partagé en consultation ») ;
- outil de diagnostic du développeur sur sa propre machine ;
- amont réel des tests d'intégration du plan de données.

### Frontière de confiance

Le jeton est une **capacité PLPT** : vérifiée par PLPT, consommée par PLPT, et elle
ne franchit jamais la frontière vers le service local.

- `redact_capability()` retire, avant transmission : le paramètre de requête portant
  le nom de la capacité, le cookie homonyme, l'en-tête `X-PLPT-Token`, et le
  paramètre homonyme présent dans `Referer`. Tout le reste de l'en-tête passe octet
  pour octet ; la sortie n'est jamais plus longue que l'entrée.
- Les sous-ressources deviennent autorisables sans répéter le jeton dans les URL :
  après autorisation, PLPT dépose la capacité en cookie dans la **première réponse**
  du flux — `Set-Cookie: <nom>=<jeton>; Path=/; HttpOnly; SameSite=Strict`, sans
  `Max-Age` (cookie de session : la capacité meurt avec le navigateur de l'invité).
  Le dépôt est supprimé quand la capacité venait déjà d'un cookie (rien à redéposer)
  et quand le flux est un upgrade WebSocket (la réponse `101` n'est jamais modifiée).
- Renoncement explicite, jamais silencieux : si les en-têtes de la première réponse
  dépassent la borne, le cookie n'est pas déposé, les octets réels sont transmis,
  `grants_failed` est incrémenté et un avertissement est journalisé.

### Option 4 écartée

Terminer TLS sur l'hôte pour un service local exigerait une autorité de certification
PLPT installée sur le poste de l'invité, donc une élévation de confiance permanente
et une interception du trafic — interdites par le modèle de menace du maître.

## Conséquences et limites assumées

- Le proxy reste en **HTTP clair**. La capacité et les données circulent sans
  chiffrement sur le LAN. Ce n'est pas acceptable pour une session réelle : le
  transport sécurisé fait l'objet d'une ADR dédiée (transport/QUIC-TLS, à livrer).
  Tant qu'elle n'est pas appliquée, le proxy ne doit servir que sur un LAN de
  confiance, et l'interface doit le dire.
- Le cookie de capacité n'est pas marqué `Secure` (pas de TLS disponible). `HttpOnly`
  et `SameSite=Strict` limitent l'exposition côté page, pas l'écoute du réseau.
- Un service local qui exige lui-même un paramètre nommé `plpt` serait privé de ce
  paramètre. Le nom est configurable (`token_name`) : la collision se résout en
  changeant le nom, jamais en désactivant le retrait.
- Inchangé et déclaré **PARTIEL** : pas de HTTP/2, pas de demi-fermeture, pas de
  limitation de débit par flux, multicast inter-machines non qualifié.
- Aucune performance n'est revendiquée : les compteurs (`tokens_redacted`,
  `grants_issued`, `grants_failed`, octets, connexions) sont des compteurs, pas une
  qualification de charge. La charge 5 invités / 30 FPS / 720p / p95 < 150 ms reste
  **NON QUALIFIÉE**.

## Preuves

- `tests/unit/test_main.cpp :: capability_is_never_forwarded_upstream` — réécriture
  exacte de la cible (`/app?a=1&plpt=…&b=2` → `/app?a=1&b=2`, `/?plpt=…` → `/`),
  cookie de capacité retiré seul puis ligne entière supprimée, `X-PLPT-Token`
  supprimé, `Referer` nettoyé, en-tête sans capacité reproduit octet pour octet,
  homonymes (`xplpt`, `plpt2`) intacts, sortie jamais plus longue, insertion
  `Set-Cookie` avant la ligne vide avec corps intact, provenance cookie/URL/en-tête
  distinguée.
- `tests/integration/platform_tests.cpp :: lan_data_plane_proxy_token_gate` —
  l'amont de test renvoie désormais **tout l'en-tête qu'il a réellement reçu** : le
  jeton n'y figure dans aucun des quatre modes d'autorisation ; `Set-Cookie` est
  présent pour une autorisation par URL ou par en-tête, absent pour une autorisation
  par cookie ; `tokens_redacted ≥ 4`, `grants_issued ≥ 3`, `grants_failed = 0` ;
  l'adresse liée est celle imposée par le test, et aucun événement ne contient le
  jeton.
- Profils d'exécution : ASan+UBSan (fuites comprises) et `_GLIBCXX_ASSERTIONS`,
  33 cas unitaires / 20 882 vérifications et 10 cas plateforme / 513 vérifications,
  0 échec dans les deux profils.
