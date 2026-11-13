# Enveloppe PLPT expérimentale — version 1.0

**Statut : modèle/codec local testé. Aucun protocole réseau interopérable ni
handshake livré.** Les types ne sont pas une autorisation. Le contrat devra être
revue/versionné avant la première connexion QUIC.

## Layout canonique

Entiers non signés, ordre réseau **big-endian**, aucun padding C++ transmis.
Header fixe de **56 octets** ; un appel decode traite exactement une enveloppe.

| Offset | Octets | Champ |
|---:|---:|---|
| 0 | 4 | magic `PLPT` (`50 4c 50 54`) |
| 4 | 2 | major = 1 |
| 6 | 2 | minor = 0 |
| 8 | 2 | type |
| 10 | 2 | reserved = 0 |
| 12 | 8 | session_id |
| 20 | 8 | participant_id |
| 28 | 4 | stream_id (0..5 dans ce modèle) |
| 32 | 8 | sequence |
| 40 | 8 | timestamp_monotonic_ms |
| 48 | 4 | payload_length |
| 52 | 4 | flags = 0 |
| 56 | N | payload opaque |

Le décodeur vérifie taille minimale, magic, champs réservés, version exacte,
valeur du type, taille maximum et longueur totale **avant** de copier le payload.
Les octets surnuméraires et les troncatures sont refusés. Aucune structure C++
n'est castée directement sur les octets reçus.

## Types et limites actuelles

| Type | Valeur | Max payload |
|---|---:|---:|
| Hello | 1 | 4096 |
| Auth | 2 | 4096 |
| SessionControl | 3 | 8192 |
| InputKeyboard / InputPointer | 4 / 5 | 1024 |
| VideoFrame | 6 | 262144 |
| VideoControl | 7 | 4096 |
| Chat | 8 | 8192 |
| Presence | 9 | 2048 |
| Feedback | 10 | 16384 |
| Heartbeat | 11 | 256 |
| Error | 12 | 4096 |

Hello utilise session/participant nuls. Les autres types exigent une session non
nulle ; hors Hello/Auth, participant et séquence doivent être non nuls. Les versions
inconnues, y compris une mineure non négociée, sont rejetées. Le schéma des payloads
et la négociation d'extensions restent à définir. Le corps chat métier est limité
à 4096 octets, indépendamment du plafond de son enveloppe.

## Ce que ce codec ne fait PAS

- Pas de TLS, d'intégrité cryptographique, d'identité hôte ni d'invitation.
- Pas de preuve d'identité via participant_id.
- Pas de compteur anti-rejeu par connexion/flux.
- Pas de validation des événements clavier, IME, pointeur ou des fragments vidéo.
- Pas de parsing incrémental d'un flux QUIC ni de fragmentation DATAGRAM.
- Pas de négociation codec, routage réel ou reconnexion.

Le transport futur doit borner sa réception **avant** d'appeler ce codec : vérifier
le header dans un buffer limité, puis attendre au plus le payload autorisé. Recevoir
un gigaoctet dans un vector avant `decode_envelope` annulerait cette protection.

Les flux logiques (contrôle, clavier, pointeur, vidéo, chat, présence) du maître sont
conservés comme concept. Le mapping type/canal, les quotas par connexion et l'ordre
applicatif doivent être spécifiés et testés avec la pile choisie. Aucun input ne
sera accepté en 0-RTT, même si le paquet est syntaxiquement valide.

Tests actuels : golden bytes, limites, type/flags/version inconnus, toutes les
troncatures d'un message, mutations déterministes et cible libFuzzer roundtrip.
Voir [preuves](../reports/VALIDATION.md). Cela n'est pas un audit du futur protocole.
