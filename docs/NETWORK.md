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

Le transport charge OpenSSL 3 à l'exécution et exige TLS 1.3 : bibliothèque
système sous Linux, DLL UCRT64 épinglées livrées à côté de l'application sous
Windows (chargées depuis son dossier seulement). Le contrôle d'indisponibilité
échoue fermé ; aucun repli TCP clair n'est permis quand TLS est requis. Les preuves
sont en boucle locale (Linux historique, Windows 2026-11-21) : ni test physique
LAN, ni audit cryptographique indépendant.

Sur ce canal circulent aussi le chat, les messages privés et, depuis le protocole
1.5, les signalements d'anomalie (invité → hôte seulement, accusé à l'auteur seul ;
ADR-019).

## 4. Proxy applicatif

`ServiceProxy` est un pont HTTP applicatif entre un invité et un endpoint local
sélectionné, avec jeton de capacité, limites et gestion de réponses HTTP/1.x,
`1xx` et WebSocket documentée dans les tests existants. **Il ne fait plus partie du partage**
(ADR-018) : il livrerait aux invités le HTML, le JavaScript et les ressources du
projet. Son trafic n'est pas chiffré ; ne l'exposez pas à un réseau non fiable.

## 5. Partage : navigateur hôte, canal média, entrées (ADR-018)

Chaîne vérifiée en boucle locale sous Windows :

`WebView2 hors écran → images JPEG du compositeur → TLS 1.3 PSK par invité →
décodage WIC → texture SDL3 → souris/clavier/texte → autorisation côté hôte →
injection dans le seul contexte navigateur concerné`.

- **Sortie réseau du navigateur** : uniquement par EgressGuard, un proxy SOCKS5
  DevLink qui n'ouvre que les couples `hôte:port` autorisés (boucle locale
  comprise). Mesure : 0 connexion vers un port canari interdit. Navigations,
  redirections, sous-ressources, fenêtres, téléchargements et schémas externes
  hors allowlist sont refusés.
- **Canal média** : une connexion TCP par invité, sur la même interface que le
  contrôle et un port choisi par l'OS ; clé PSK de 32 octets propre au
  participant, offerte uniquement sur le contrôle déjà chiffré ; enregistrements
  bornés avant allocation ; une seule image en attente par invité.
- **Entrées** : chaque enregistrement est réautorisé par `SharePolicy` (participant,
  révocation, séquence strictement croissante, suspension, mode, bail) ; refus
  systématique en mode C, même pour un client modifié.
- **Limites** : viewport fixe 1280×720 ; invité Linux non disponible ; aucune
  mesure sur deux machines.

## 6. Tests et limites

- `devlink-platform-tests` : plan de contrôle réel, TLS 1.3, messagerie et
  signalements en boucle locale. `devlink-browser-tests`, `devlink-share-tests`,
  `devlink-desktop-e2e-tests` (Windows) : navigateur réel, partage, attaques, cinq
  invités et bout en bout sur les vrais exécutables. Aucun ne remplace une
  validation LAN multi-machine (`docs/reports/REAL_LAN_QUALIFICATION.md`).
- `DemoBrowserBackend` est explicitement synthétique et réservé au parcours
  `--demo-synthetic-browser` ; il n'est jamais partageable.
- L'absence d'IPv6, d'un runtime, d'un second ordinateur ou d'un écran physique
  reste `SKIP` (code 77) ou non qualifiée, jamais un PASS.
- État vérifié et prochaines portes : `docs/reports/IMPLEMENTATION_STATUS.md`.
