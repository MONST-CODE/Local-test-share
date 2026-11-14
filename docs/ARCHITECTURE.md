# PLPT — conception technique du premier incrément

Date : 2026-11-13. Statut : **moniteur local et socle**, pas une V1 de partage.
Référence normative : [document maître v1.0](ARCHITECTURE_MASTER.md).

## 1. Choix de livraison

La qualité ne consiste pas à écrire un squelette pour chaque rectangle du schéma
puis à marquer les lots terminés. Le premier chemin utilisable est :

```text
API OS en lecture seule -> observation normalisée -> identité de processus
    -> registre de candidats -> snapshot immuable -> CLI / SDL3 + ImGui
```

Ce chemin permettait d'**examiner et sélectionner localement** un endpoint sans
aucun effet réseau. Depuis l'incrément « réseau LAN réel », un second chemin est
opérationnel et distinct :

```text
code d'invitation (CSPRNG) -> annonce UDP multicast -> connexion TCP de contrôle
    -> approbation de l'hôte -> session/messagerie -> proxy TCP à jeton
    -> service local réel (HTTP et WebSocket)
```

La sélection locale d'un endpoint n'est toujours pas une permission réseau : seul
l'hôte, en publiant explicitement un service dans une session ouverte, crée un
accès. Le proxy exige le jeton de service comme capacité ; la découverte ne donne
ni identité ni droit.

Les modèles anticipés de F/G/H sont compilés dans `plpt_models` pour leurs tests,
mais ne sont pas liés aux exécutables installés. Ils se trouvent dans le namespace
`plpt::model`. Il n'existe pas de simulation de TLS derrière une UI « connecté ».

## 2. Découpage et dépendances

```text
app/host_shell             app/desktop_ui
     CLI                       SDL3 + ImGui
       \                       /
        engine/discovery/monitor     (worker + commandes bornées)
                    |
        service_registry + process_tree + terminal_classifier
                    |
        platform/platform.hpp       (interfaces / observations / erreurs)
             /             \
    Windows natives     Linux proc / getifaddrs

Modèles de session (liés au poste de contrôle et aux tests, jamais au CLI) :
    session -> messaging
    protocol (codec borné, enveloppe 56 octets, trames TCP)
    resource_manager (estimation, sans collecte CPU/GPU)

Réseau LAN réel (engine/net, engine/platform/socket, engine/core/digest+secrets) :
    wire        trames et messages typés, assembleur borné
    discovery   annonceur/écouteur UDP multicast 239.255.77.88:47788
    host_server plan de contrôle hôte : authentification, approbation, révocation
    guest_client plan de contrôle invité : attente d'autorisation, messagerie
    proxy       plan de données : pont TCP autorisé par jeton de service

Interfaces non implémentées : browser, ITemporaryStorage.
```

Le cœur ne dépend pas d'ImGui, du renderer ou d'une API COM. Les règles de session
ne connaissent pas Winsock. Aucun adaptateur QUIC/Docker/WebView2 n'est masqué
dans les modules de découverte.

L'ancien `engine/transport/transport.hpp` (interface `ITransport` sans
implémentation) a été **supprimé** : il est remplacé par `engine/platform/socket.hpp`
(sockets réelles, POSIX et Winsock) et `engine/net/*`. Aucune abstraction de
transport sans implémentation n'est conservée.

Le futur découpage en processus `Desktop UI / Host Engine / Browser Worker` et
IPC à ACL utilisateur du maître reste la cible. **Le moniteur actuel utilise un
worker dans le même processus**, pas un IPC prétendument sécurisé. Avant de traiter
une entrée invitée, l'orchestrateur et les frontières IPC devront être implémentés.

## 3. Contrats du cœur

### Types et erreurs

`SessionId`, `ParticipantId`, `ProcessId`, `ContextId`, `ServiceId`, `Port`, etc.
sont des types distincts, comparables, sans conversion implicite entre identités.
Zéro signifie « non attribué » ; les interfaces métier le valident explicitement.
Les agrégats représentent des observations, pas des objets intrinsèquement authentifiés.

`Result<T>` est `[[nodiscard]]`. Les échecs attendus sont des valeurs avec code,
opération et code OS. Un accès à une valeur absente échoue explicitement
(`bad_optional_access`) au lieu de provoquer un comportement indéfini. Construire
`failure(Ok)` est une erreur de programmation. L'épuisement mémoire reste une
exception fatale du runtime : aucun faux succès ni `catch (...)` silencieux.

`IClock` permet des tests déterministes d'expiration ; les durées utilisent une
horloge monotone. Les comparaisons utilisent une différence, pas une addition
susceptible de déborder. `SecureRandom` appelle BCryptGenRandom / getrandom,
avec gestion EINTR sous Linux et sans fallback pseudo-aléatoire.

### Observation partielle

`Observation<T>` contient `records`, `complete`, `warnings`. Une table vide et
complète peut signifier une disparition ; une erreur de table **ne le peut pas**.
Le registre distingue `Gone` d'`Unavailable`. Un PID peut rester inconnu même
lorsque la table réseau est complète : droits insuffisants, processus disparu,
identité de création inaccessible ou plusieurs propriétaires d'un inode.

### Identité et corrélation

1. Prendre un snapshot de processus A.
2. Lire les tables sockets (elles ne sont pas atomiques entre familles).
3. Prendre un snapshot de processus B.
4. Associer un PID seulement si l'heure de création est connue et identique en A/B.
5. Construire les liens PPID ; rejeter un parent créé après l'enfant et arrêter les cycles.

Cela réduit les mauvaises associations de PID recyclés. Ce n'est ni une transaction
OS, ni une preuve cryptographique : un changement de socket entre deux lectures
reste possible. Le classificateur n'est jamais une source d'autorisation.

Les noms sont comparés exactement, sans chercher `bash` dans n'importe quelle
chaîne. Un runner proche ne masque pas un terminal plus haut dans la filiation.
Les alternatives et les indices restent disponibles. Une origine « probable »
ne prouve pas le projet : le champ projet reste **non attribué**, sans lire `.env`,
ligne de commande, historique ou variables d'environnement.

## 4. Adaptateurs système

### Windows x64 — priorité

- `GetExtendedTcpTable`, classe `TCP_TABLE_OWNER_PID_ALL`.
- `GetExtendedUdpTable`, classe `UDP_TABLE_OWNER_PID`.
- Quatre layouts distincts : TCP4, TCP6, UDP4, UDP6 ; scopes IPv6 conservés.
- Allocation de table jusqu'à 16 MiB, cinq tentatives si sa taille change ; validation
  du nombre d'entrées avant parcours ; échec visible si table instable/inaccessible.
- `CreateToolhelp32Snapshot` / `Process32FirstW` / `Process32NextW` : le PPID vient
  de **`th32ParentProcessID`**, sans dépendance à NtQueryInformationProcess.
- `GetProcessTimes` et `QueryFullProcessImageNameW` avec
  `PROCESS_QUERY_LIMITED_INFORMATION`. Handles RAII. Échec d'accès = champs absents.
- UTF-16 -> UTF-8 avec taille exacte, sans écrire le terminateur au-delà du buffer.
- `GetAdaptersAddresses` pour les interfaces et adresses, y compris loopback.

L'ancien contrôle syntaxique croisé est historique et son helper a été retiré.
La correction Windows utilise exclusivement le **clang++.exe de MSYS2 UCRT64**,
cible GNU x64, MinGW-w64/UCRT et GNU libstdc++, sans SDK/ABI MSVC ni LLVM autonome.
Les résultats natifs Windows UCRT64 sont suivis dans
[le rapport de validation UCRT64](reports/UCRT64_WINDOWS_VALIDATION.md) ; le rapport
Microsoft antérieur est conservé comme profil remplacé.
Cela ne remplace pas les tests de churn, de compte standard, DPI/IME et applications
réelles décrits dans la roadmap.

### Linux — secondaire

`/proc/net/{tcp,tcp6,udp,udp6}` + inodes `/proc/<pid>/fd`, sans deuxième lecture
positionnelle des tables : un inode reste attaché à la ligne qui l'a fourni.
Deux processus observés pour le même inode donnent un propriétaire inconnu,
pas « le premier PID trouvé ». `from_chars` valide taille, base et débordement.
Les états TCP FIN/SYN/ESTABLISHED ne deviennent pas des listeners ; un UDP
connecté n'est pas traité comme un endpoint Web candidat.

`/proc/<pid>/stat` est découpé après la **dernière parenthèse** du nom de processus.
Le champ 22 est starttime (pas le champ 23, vsize). Les permissions et processus
qui disparaissent sont tolérés. `getifaddrs` fournit les interfaces réelles.
Limites : namespace réseau courant, visibilité partielle `/proc`, attribution
multi-propriétaire inconnue, scope des endpoints IPv6 non fourni par ces tables.
Un backend netlink pourra remplacer ce choix sans modifier le modèle.

## 5. Registre et sélection

Un groupe stable est indexé par `(PID, création)`, et peut contenir plusieurs
endpoints. Les observations sans identité vérifiable sont regroupées par endpoint.
Le futur graphe de projet pourra relier plusieurs groupes : aucune relation
1 processus = 1 projet n'est imposée.

- Nouveau service : `Detected`, jamais `Authorized`.
- Sélection : un **endpoint TCP précis**, pas tous les ports du processus.
- Endpoint sélectionné disparu : sélection invalidée, même si un autre port reste.
- PID recyclé : nouvel identifiant ; aucune sélection transmise au nouveau processus.
- Retour après disparition : nouvel identifiant.
- Observation partielle : indisponible, sélection retirée.
- Propriétaire inconnu : pas de conservation automatique de sélection au prochain snapshot.
- Tombstones : une observation ; entrées anciennes indisponibles bornées.

Le registre accepte au plus 4096 groupes et 4096 endpoints par groupe. Les anciens
indisponibles sont évincés lorsque nécessaire. Ce sont des garde-fous, pas des
objectifs de charge démontrés. `Authorized` appartient au vocabulaire futur,
mais **aucun appel de ce registre n'active cet état**.

## 6. Concurrence et UI

`Monitor` possède les adaptateurs et le registre sur un `std::jthread`. La file
contient au maximum 32 commandes typées (`Refresh`, `Select`, `Protect`). L'UI ne
lit pas le registre mutable : elle récupère un `shared_ptr<const MonitorView>`.
La dernière vue remplace la précédente ; pas de file de snapshots illimitée.

Le worker effectue les appels OS. La boucle ImGui n'attend pas un inventaire ni
un handshake. La destruction demande l'arrêt, réveille le worker et le rejoint
avant de détruire mutex/providers. Une API OS déjà engagée n'est pas annulable :
le temps d'arrêt dépend de la fin de cette lecture. Le rafraîchissement est de
2 secondes ; ralentissement adaptatif en arrière-plan encore à ajouter.

SDL3 crée la fenêtre ; les backends officiels ImGui SDL3 et SDLRenderer3 dessinent
le moniteur. Ce choix n'arrête pas le renderer GPU du futur flux vidéo (ADR-012).
Navigation locale, filtre, table, inspection et messages d'erreur sont utilisables.
DPI initial et changement de display scale sont pris en compte, mais les tests
multi-écran Windows restent nécessaires. INI et log ImGui sont désactivés.

## 7. Réservation de port — prototype F

`DynamicPortAllocator` crée un socket, configure l'exclusivité, appelle `bind(0)`,
`getsockname` puis conserve le socket dans un objet RAII déplaçable. Le handle
Windows a la largeur de `SOCKET` (`UINT_PTR`), pas celle d'un `int`.

- Aucun scan « libre puis bind ».
- `SO_EXCLUSIVEADDRUSE` vérifié avant bind sous Windows ; pas de `SO_REUSEADDR`.
- Descripteurs non héritables ; IPv6 `V6ONLY` explicite.
- Trois essais bornés avec de nouveaux sockets et toujours port zéro.
- Scope loopback par défaut ; LAN exige une demande explicite.
- Le cas TCP appelle réellement `listen`, mais n'est pas un fallback transport produit.

Le moniteur ne demande **aucune** réservation. Les tests font du bind concurrent
sur le port réellement réservé pour prouver l'exclusivité, puis sa libération.
Ils ne mettent pas de serveur HTTP sur un port fixe.

**Point d'intégration QUIC :** une pile telle que MsQuic peut posséder ses sockets
elle-même. Si elle ne peut pas adopter un socket prélié, elle devra faire elle-même
`port=0 + query endpoint + rétention`. Il est interdit de fermer notre réservation
pour rebinder « le même port libre » dans la pile : ce serait réintroduire TOCTOU.

## 8. Modèles de session et réseau LAN réel

`Session` valide les transitions, l'expiration exacte, le plafond de participants,
les capacités et la liaison contexte. Les invités ne peuvent pas devenir Host.
Un contexte indépendant n'est pas réattribué ; un contexte partagé dispose d'un
seul contrôleur, explicitement accordé. Révocation/reconnexion/clôture retirent
les droits pertinents immédiatement **dans ce modèle**.

`admit_verified_guest` reste une frontière interne de confiance ; elle est
désormais appelée par un **handshake réel** (`engine/net/host_server.cpp`) : code
d'invitation de 80 bits tiré du CSPRNG de l'OS, comparaison à temps constant, limite
de tentatives par adresse et par connexion, puis — si l'approbation est requise —
décision explicite de l'hôte avant toute admission. Aucun 0-RTT, aucune reprise sans
code ; une reconnexion dans le délai de grâce rend la même place, pas une nouvelle.

Il n'y a toujours **ni TLS, ni preuve d'identité de l'hôte par clé publique** : le
canal de contrôle circule en clair sur le LAN et l'identité de l'hôte repose sur
l'adresse source observée et sur le code communiqué hors bande. Le jeton de service
est une capacité d'URL, pas un chiffrement. Ce sont des limites déclarées.

### États réels de l'incrément « réseau LAN »

| Composant | État | Preuve |
| --- | --- | --- |
| `platform/socket` (TCP/UDP, poll, multicast, `MSG_NOSIGNAL`) | **OPÉRATIONNEL** | cas `platform`, `lan_*` |
| `core/digest` (CNG, AF_ALG, sinon dlopen libcrypto) | **OPÉRATIONNEL** | vecteurs FIPS SHA-1/SHA-256 |
| `core/secrets` (codes d'invitation, jetons, masquage) | **OPÉRATIONNEL** | `secrets_digests_and_formats_are_exact` |
| `net/wire` (trames, assembleur borné, gardes anti-DoS) | **OPÉRATIONNEL** | `wire_messages_roundtrip_and_guard` |
| `net/discovery` (multicast, broadcast, saisie manuelle) | **OPÉRATIONNEL** en loopback ; **PARTIEL** : multicast inter-machines non qualifié | `lan_discovery_announce_and_listen` |
| `net/host_server` (authentification, approbation, messagerie, révocation, délais, fermeture) | **OPÉRATIONNEL** sur TCP réel | `lan_control_plane_host_and_guest` |
| `net/guest_client` (attente d'autorisation, services, messagerie, latence mesurée) | **OPÉRATIONNEL** sur TCP réel | idem |
| `net/proxy` (pont TCP à jeton, HTTP et WebSocket transparents, 403/502/503) | **OPÉRATIONNEL** en loopback ; **PARTIEL** : pas de HTTP/2, pas de demi-fermeture, pas de débit limité par flux | `lan_data_plane_proxy_token_gate` |
| `app/demo_service` (HTTP/1.1 + RFC 6455, écho) | **OPÉRATIONNEL** ; construit, non exécuté par les tests CI | qualifié par sonde locale |
| Interface SDL/ImGui branchée sur `host_server`/`guest_client` | **NON IMPLÉMENTÉ** | le tableau de bord n'ouvre pas encore de session |
| Enchaînement détection native → service partagé (`monitor` → `proxy`) | **NON IMPLÉMENTÉ** | la détection existe, le câblage n'est pas fait |
| Entrée clavier/souris, flux vidéo, TLS, orchestrateur multi-processus | **NON IMPLÉMENTÉ** | aucun stub silencieux |

`Messaging` utilise cette session pour vérifier acteur et destinataire. Une réponse
à un DM reste dans le DM. La clé de canal est une paire d'IDs 64 bits, pas une
concaténation tronquée. Quotas : 4096 octets/message, 10 créations/seconde/acteur,
1024 éléments et 1 MiB de corps par session. UTF-8 strict ; lecteurs par participant.
Les routes de feedback sont **omises par défaut**, car un secret peut être dans le
chemin aussi bien que dans la query. Captures non implémentées = refus explicite.
Mentions/réactions/épinglage/typing et modération complète restent à développer.

Le codec d'enveloppe vérifie le header et la taille avant allocation ; il ne prouve
ni l'identité, ni l'ordre de réception, ni l'autorisation. Ces contrôles devront
s'appliquer à chaque flux authentifié dans le futur orchestrateur.

`resource_manager` est une heuristique non calibrée ; NaN, infinis et valeurs
incohérentes sont refusés. Aucun chiffre CPU/RAM/FPS n'est prétendu mesuré ici.

## 9. Fermeture future et limites

Un `CleanupReport` incomplet interdit au modèle de déclarer la session `Closed`.
Cela ne nettoie pas encore un navigateur : `ITemporaryStorage` n'a qu'un backend
indisponible. Le sweep récursif par préfixe a été retiré. L'orchestrateur devra :
révoquer -> arrêter entrées/flux -> invalider jetons -> arrêter workers navigateur
-> vérifier les leases/processus -> supprimer les profils possédés -> publier le
rapport. Les données de messagerie doivent être explicitement vidées par cet
orchestrateur ; le modèle de session seul ne les efface pas.

Le protocole de nettoyage devra refuser les symlinks/reparse points et les chemins
non possédés, traiter les fichiers verrouillés, et vérifier les orphelins après
crash. Libérer un vector ou supprimer un fichier n'est pas un effacement physique
garanti. Pagefile, SSD, logs OS et crash dumps restent dans le modèle de menace.

Voir [sécurité](SECURITY.md), [portes de validation](ROADMAP.md) et
[preuves réelles](reports/VALIDATION.md) avant d'évaluer l'avancement d'un lot.
