# Protocole DevLink — état réel du contrôle et limite média

**État du codec courant :** enveloppe de contrôle 1.3 (`engine/protocol/envelope.*`).
Ce document remplace les anciennes affirmations « version 1.0 sans handshake » de
la version historique de ce fichier. Le codec teste la syntaxe ; il ne suffit pas à
autoriser une opération réseau.

## Enveloppe de contrôle

Les entiers sont sérialisés en ordre réseau big-endian ; aucune structure C++ native
n'est copiée telle quelle. L'en-tête fixe fait 56 octets :

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 4 | magic `PLPT` (`50 4c 50 54`) |
| 4 | 2 | version majeure (1) |
| 6 | 2 | version mineure (courante 3) |
| 8 | 2 | type de message |
| 10 | 2 | réservé, doit être zéro |
| 12 | 8 | session ID |
| 20 | 8 | participant ID |
| 28 | 4 | stream ID |
| 32 | 8 | séquence |
| 40 | 8 | horodatage monotone |
| 48 | 4 | longueur payload |
| 52 | 4 | flags (actuellement zéro) |
| 56 | variable | payload typé et borné selon le message |

Le décodeur vérifie la longueur exacte, les champs, la version et la limite du type
avant de copier le payload. Les plafonds incluent notamment 256 KiB pour le type
`VideoFrame`, mais ce plafond de codec n'est pas une implémentation de flux vidéo.

## Admission et protection du plan de contrôle

Le code du plan de contrôle a ajouté les types 16–18 : `TlsUpgrade`,
`AuthChallenge` et `AuthProof`. Le parcours visé/implémenté dans le moteur est :

1. Hello public sans code d'invitation ni secret de reprise transmis en clair ;
2. nonce aléatoire de l'hôte ;
3. preuve HMAC de possession du code ou du secret de reprise, liée au nonce et à
   la session ;
4. notice d'upgrade sans secret ;
5. poignée TLS 1.3 PSK avant l'admission et les échanges applicatifs.

Les preuves loopback sont Linux/OpenSSL et ne représentent ni un test physique LAN
ni un audit de cryptographie indépendant. Le transport TLS Windows n'est pas
opérationnel/qualifié. Voir `engine/net/transport*`, `engine/net/wire.*`,
`docs/ADR/ADR-016-transport-tls13-psk.md`, `docs/ADR/ADR-017-defi-reponse-et-basculement-tls13.md`
et la preuve datée de basculement.

## Messages et limites

Les types de base couvrent Hello/Auth, état/contrôle, entrée, frame réservée,
chat, présence, feedback, heartbeat et erreurs. Les types 13–18 ajoutent l'état de
session, liste de services, départ, upgrade TLS, défi et preuve. Les tailles sont
centralisées dans `max_payload_size()` ; l'encodeur/décodeur versionné et les
payloads typés se trouvent dans `engine/net/wire.*`.

Ces vérifications ne prouvent pas à elles seules l'identité de l'appelant, son rôle,
sa capacité ou son droit à une entrée. Ces décisions doivent être vérifiées par le
moteur de session et attachées au canal réellement authentifié.

## Codec média v1 présent, pipeline de partage absent

`engine/browser/remote_frame.hpp` définit maintenant un codec de payload versionné
à entiers big-endian, avec magic/header explicites, identité de session/participant,
stream/contexte, séquence, timestamp monotone, dimensions, stride et taille déclarée.
La version 1 accepte uniquement des frames complètes RGBA8 sans compression, jusqu'à
4096×2160 et avec une taille exacte `stride × height`. Les inputs ont un codec
versionné distinct, coordonnées normalisées Q0.16, champs réservés contrôlés et
texte UTF-8 borné à 32 octets. Des tests négatifs et une entrée fuzz exercent ces
parseurs ; ils n'établissent pas une connexion de production.

Ce codec ne traverse **pas encore** l'enveloppe réseau/TLS et n'est relié ni à une
capture WebView2, ni à un encodeur/décodeur compressé, ni à une texture invitée, ni à
l'autorisation d'input côté serveur. Le plafond du protocole de contrôle est distinct
du plafond du codec média : ne pas envoyer une frame par un canal qui ne supporte pas
explicitement sa taille. Il n'y a donc toujours pas de pipeline média livré.

Le vrai Browser Worker WebView2, la politique de navigation effective, le pipeline
de pixels, le retour d'interactions et les tests A/B/C sur réseau restent à
implémenter. Ne présentez aucun rendu synthétique comme une capture du navigateur
réel.

## Tests existants

Les tests unitaires et platform couvrent des encodages, limites, erreurs, défi-
réponse, TLS loopback, mauvais secret et refus de runtime absent. Les références et
leurs limites sont consignées dans `docs/reports/FINAL_COMPLETION_AUDIT.md` et les
rapports d'évidence. GCC exploratoire ne remplace pas les builds officiels Clang++.
