# Architecture réseau de DevLink — état audité

Ce document sépare le code présent des fonctions qui restent à construire. La
présence d'un modèle réseau, d'un test loopback ou d'un écran n'est pas une preuve
d'un partage LAN complet.

## 1. Services locaux et ports

Le moniteur observe les processus et ports existants. Une détection est informative
seulement : elle n'autorise pas le partage. Le moteur utilise des sockets allouées
par l'OS et des adresses de bind explicites selon sa configuration. L'interface,
les règles de pare-feu, les interfaces multiples et le test multi-machine doivent
être qualifiés sur la plateforme cible.

## 2. Découverte

Le dépôt contient annonce/écoute LAN ainsi qu'un parcours de test local. Les
annonces ne sont pas une authentification de l'hôte et ne doivent contenir aucun
secret. Multicast/broadcast entre machines physiques, réseaux Wi-Fi isolés, pare-feu
et IPv6 demeurent à qualifier ; un test loopback ne valide pas ces scénarios.

## 3. Plan de contrôle hôte/invité

Le code courant utilise un échange de défi-réponse avant l'upgrade TLS :

1. Hello avec informations publiques, sans code d'invitation ni secret de reprise ;
2. défi aléatoire de l'hôte ;
3. preuve HMAC vérifiée par l'hôte ;
4. notice d'upgrade contenant les paramètres publics nécessaires ;
5. poignée TLS 1.3 PSK avec identité liée à la session ;
6. admission/état/chat sur le canal TLS selon le cycle de session.

Le code de transport charge OpenSSL à l'exécution sous Linux et exige TLS 1.3. Le
contrôle d'indisponibilité échoue fermé ; aucun fallback TCP clair n'est permis
quand TLS est requis. Le transport TLS Windows n'est pas câblé/qualifié : ne pas
annoncer le plan de contrôle chiffré comme disponible sur Windows. Les preuves
loopback consignées dans `docs/reports/evidence/2026-11-18-etape5-basculement-tls13.txt`
ne sont ni un test physique LAN ni un audit cryptographique indépendant.

## 4. Proxy applicatif

`ServiceProxy` est un pont HTTP applicatif entre un invité et un endpoint local
sélectionné, avec jeton de capacité, limites et gestion de réponses HTTP/1.x,
`1xx` et WebSocket documentée dans les tests existants. Il ne capture pas une page
WebView et ne fournit pas le partage d'images. Le trafic du proxy n'est pas établi
comme chiffré de bout en bout dans l'état courant ; ne l'utilisez pas sur un réseau
non fiable. La politique d'origine et le confinement des sous-ressources d'un vrai
navigateur ne sont pas implémentés.

## 5. Browser Worker, frames et interactions — non disponibles

`engine/browser/webview2_backend.cpp` échoue explicitement : création du contrôleur,
navigation réelle, capture et injection WebView2 ne sont pas implémentées. Le backend
géré génère du contenu synthétique et est réservé aux tests/démos explicites. Aucun
invité ne reçoit actuellement un rendu navigateur réel.

`engine/browser/remote_frame.hpp` fournit maintenant un codec de payload v1
portable (big-endian) et borné pour les frames complètes RGBA8 et les événements
input ; `engine/media/frame_queue.hpp` ajoute une file bornée avec abandon des frames
complètes périmées. Ces composants ne sont pas connectés à un transport média/TLS,
à WebView2 ou au renderer invité et ne constituent pas encore un flux réseau supporté.

Il n'existe pas de chaîne vérifiée :

`WebView2 → pixels → encodage → TLS → invité → décodage → texture SDL3`.

De même, les interactions ne sont pas injectées dans un navigateur réel. Les rôles
et permissions du modèle de session ne prouvent pas encore un parcours A/B/C
complet sur connexions réseau forgées.

## 6. Tests et limites

- Les suites loopback testent le protocole, le transport Linux et le proxy sans
  remplacer une validation LAN multi-machine.
- `DemoBrowserBackend` est explicitement synthétique, pas un navigateur ; la fabrique
  dédiée `create_demo_browser_backend()` ne sert qu'au parcours `--demo-synthetic-browser`.
- `BrowserPolicy` compare désormais les origines HTTP(S) canoniques exactement (sans
  préfixe ni joker) et rejette userinfo/ports invalides/IP non canoniques. Ce
  validateur unitaire ne filtre pas encore les navigations, redirections ou
  sous-ressources d'un WebView2 réel, puisque ce backend n'existe pas.
- L'absence d'IPv6, d'un runtime Windows, d'un second ordinateur ou d'un écran
  physique doit rester `SKIP`, `BLOCKED` ou `IMPLEMENTED_BUT_NOT_VERIFIED`, jamais
  être transformée en PASS.
- Les statuts détaillés et les prochaines portes figurent dans
  `docs/reports/FINAL_COMPLETION_AUDIT.md`.
