# Architecture Réseau de DevLink

Ce document décrit le modèle réseau, les protocoles de transport, la découverte locale et les flux de communication de **DevLink**.

---

## 1. Deux Familles de Ports Distinctes

Une distinction fondamentale régit l'architecture réseau de DevLink :

1. **Ports Projet (Amont)** :
   - Ce sont les serveurs exécutés localement par le développeur (Next.js sur 3000, Vite sur 5173, Flask sur 5000, Supabase, API, etc.).
   - DevLink n'ouvre, ne modifie et ne redirige jamais directement ces ports. Il les observe passivement via les tables de l'OS.
2. **Ports Internes DevLink (Aval)** :
   - Ce sont les endpoints réservés par DevLink pour le plan de contrôle et le plan de données.
   - Ils sont **toujours alloués dynamiquement** par le système d'exploitation via `bind(port=0)` sous le compte utilisateur courant.

---

## 2. Découverte Réseau Local (LAN)

DevLink implémente un mécanisme de découverte LAN sans dépendre d'Internet, de registre central ou de serveur cloud tiers :

- **Protocole** : UDP Multicast sur le groupe administré `239.255.77.88`, port `47788`.
- **Mécanisme de repli** : Émission en broadcast IPv4 (`255.255.255.255`) et loopback local (`127.0.0.1`) pour les environnements de test sur machine unique.
- **Sécurité de l'annonce** :
  - L'annonce contient uniquement : `session_id`, `session_name`, `host_name`, `control_port`, nombre de participants, capacité max et état d'acceptation.
  - **Le code d'invitation n'est JAMAIS diffusé sur le réseau**.
  - Une annonce LAN ne constitue en aucun cas une preuve d'identité ni une autorisation d'accès.

---

## 3. Plan de Contrôle (`HostServer` & `GuestClient`)

Le plan de contrôle gère le cycle de vie de la session, l'authentification et la messagerie :

- **Transport** : TCP sécurisé avec trames bornées (`Envelope` de 56 octets d'en-tête + charge utile validée avant allocation).
- **Poignée de main (Handshake)** :
  1. L'invité transmet un message `GuestHello` contenant son nom, son identifiant de client et le code d'invitation présenté.
  2. L'hôte compare le code en **temps constant** avec l'entropie OS générée (`invite_code_matches`).
  3. Si le code est invalide, l'hôte refuse la connexion immédiatement (`HostAuth::accepted = false`) et incrémente le compteur de tentatives suspectes.
  4. Si le code est valide et que l'hôte exige une approbation manuelle, l'invité entre dans l'état `AwaitingApproval`.
  5. Dès que l'hôte clique sur "Autoriser", un message `HostAuth` d'admission est envoyé, suivi de l'état complet de la session (`SessionState`) et de la liste des services partagés (`ServiceList`).
- **Supervision & Présence** :
  - Envoi périodique de trames `Heartbeat` avec horodatage monotone et écho.
  - La latence affichée correspond au temps d'aller-retour réel (RTT) mesuré par les sockets.

---

## 4. Plan de Données (`ServiceProxy`)

Le plan de données assure le relais applicatif entre les testeurs et l'application Web locale :

- **Contrôle d'accès par jeton de capacité** :
  - Le proxy écoute sur un port TCP dynamique dédié.
  - Chaque connexion entrante doit présenter un jeton de 32 caractères hexadécimaux (`?plpt=<token>` ou cookie).
  - Toute requête sans jeton ou avec un jeton invalide est immédiatement rejetée avec un statut HTTP 403.
- **Pont bidirectionnel transparent** :
  - Une fois autorisé, le flux est relayé de façon transparente entre l'invité et le service local amont.
  - Prise en charge native des requêtes HTTP/1.1, Server-Sent Events (SSE) et WebSockets (RFC 6455 après poignée de main Upgrade).
  - Gestion stricte de la contre-pression (backpressure) et tampons bornés à 256 Ko par flux.

---

## 5. Rendu Distant & Flux Vidéo

Pour les scénarios où le code et les assets Web ne doivent pas quitter la machine hôte :
- Le navigateur de test s'exécute sur le PC de l'hôte dans un profil temporaire dédié.
- Les frames de composition sont capturées hors écran et transmises sous forme de trames d'images au client invité.
- L'invité envoie en retour des événements d'interaction (souris et clavier) sérialisés et filtrés.
