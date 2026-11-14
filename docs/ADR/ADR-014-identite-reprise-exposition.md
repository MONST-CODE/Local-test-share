# ADR-014 — Identité du participant, reprise de place et exposition réseau

Date : 2026-11-14. Statut : décision d'implémentation de l'incrément « stabilisation »
(Phases A et B du maître). Ne vaut pas acceptation V1.

## Contexte

Le maître impose : identité liée au **handshake authentifié**, jamais au nom, à
l'adresse, au PID, au multicast ou à un identifiant auto-déclaré ; secrets de reprise
indépendants, révocables et limités dans le temps ; aucune réattribution de place au
prétexte que deux personnes portent le même nom.

État réel du code avant cette décision :

- `HostServer::admit()` réattribuait une place libérée si
  `slot.name == connection.guest_name` dans le délai de grâce (30 s). Deux testeurs
  portant le même prénom — ou quiconque connaissait le nom d'un participant —
  récupéraient sa place, son identité de session et ses capacités. Le nom est un
  affichage, pas une identité ;
- la file des demandes en attente était purgée par couple (nom, adresse), même
  ambiguïté ;
- `HostConfig::bind_address` et `ProxyConfig::bind_address` valaient
  `IpAddress::any(IpFamily::Ipv4)` **par défaut** : PLPT écoutait sur toutes les
  interfaces, y compris VPN, tunnels et interfaces virtuelles, sans que personne ne
  l'ait demandé ;
- aucune énumération réelle des interfaces n'existait. `select_lan_interfaces()`
  triait une liste fournie par l'appelant, et aucun appelant ne la fournissait : il
  était donc impossible de déterminer une interface privée LAN.

## Options

1. Conserver le couple nom + adresse source comme identité de reprise.
2. Identifiant auto-déclaré par l'invité (UUID généré côté invité).
3. **Secret de reprise aléatoire** émis par l'hôte sur le canal authentifié, à durée
   limitée, consommé à l'usage (rotation), révoqué avec la place.
4. Jeton de session persistant écrit sur le disque de l'invité.

Pour l'exposition réseau : (a) conserver `0.0.0.0` par défaut, (b) exiger une adresse
saisie à la main, (c) déterminer l'adresse par énumération réelle des interfaces avec
repli sûr et signalé.

## Décision

### Identité et reprise — option 3

- **Protocole : révision mineure 1.2.** `GuestHello` porte `resume_secret` ;
  `HostAuth` délivre `resume_secret` et `resume_ttl_ms`. Un pair annonçant une
  révision inférieure est refusé explicitement (`UnsupportedVersion`, « protocole 1.2
  requis ») : aucun repli deviné. `negotiate_version` continue d'accepter 1.0/1.1/1.2
  au niveau de l'enveloppe et refuse 1.3 ainsi que tout majeur différent.
- Le secret est **16 octets du CSPRNG de l'OS** en hexadécimal
  (`generate_resume_secret()`), émis uniquement après admission, uniquement sur le
  canal déjà authentifié par le code d'invitation. Jamais annoncé sur le LAN, jamais
  journalisé, jamais affiché : l'interface ne montre que « reprise possible pendant
  N s ».
- **Rotation à chaque admission** : le secret présenté est consommé puis remplacé. Un
  secret capturé après coup ne rejoue pas une reprise.
- Comparaison à **temps constant** et à longueur égale (`constant_time_equals`).
- La **révocation supprime l'emplacement de reprise** : un participant révoqué ne
  retrouve jamais sa place.
- **Départ volontaire ≠ coupure.** Un `Bye` libère le siège immédiatement
  (révocation du participant, compteur `left`) et ne réserve aucune reprise ; une
  coupure réserve la place pendant `reconnect_grace_ms`. Sans cette distinction, des
  testeurs quittant proprement la session consommeraient définitivement la capacité —
  un siège déconnecté reste compté par `Session::active_count()` jusqu'à révocation.
- Le nom redevient un affichage : une reprise sous un nom différent est acceptée (le
  secret fait foi) et journalisée. La purge des demandes en attente se fait par
  identifiant de demande.

### Exposition réseau — option (c)

- `bind_address` **invalide par défaut**, ce qui signifie « PLPT choisit ».
- `resolve_bind_address()` énumère les interfaces réelles (`getifaddrs(3)` sous
  Linux, `GetAdaptersAddresses` sous Windows), retient une adresse privée LAN de la
  famille demandée, **préfère une adresse routable à une adresse lien local**, et
  retombe sur la boucle locale en le disant (`loopback_only` + avertissement).
- `0.0.0.0` / `::` n'est utilisé que si la configuration l'impose ; ce choix est
  alors signalé (`any_interface`, note dans l'instantané publié et dans le journal).
- L'adresse réellement liée est publiée : l'interface ne peut pas afficher une
  adresse différente de celle qui écoute.

### Options écartées

- Option 1 : viole l'exigence d'identité ; homonymie et usurpation triviales.
- Option 2 : un identifiant auto-déclaré est copiable ; il ne prouve rien.
- Option 4 : persistance sur disque = surface de fuite supplémentaire, et la reprise
  après redémarrage du processus n'est pas un besoin exprimé. Le secret reste en
  mémoire : un processus redémarré revient comme un nouvel arrivant, ce qui est
  documenté plutôt que masqué.

## Conséquences et limites assumées

- Un invité dont le processus redémarre perd sa place et doit être ré-admis.
- Si l'OS ne fournit pas d'entropie, aucun secret n'est émis : la place n'est pas
  réservée, un avertissement est levé et le journal dit « ne pourra pas reprendre sa
  place ». Dégradation visible, jamais silencieuse.
- Le canal reste **en clair** : le secret de reprise circule sans chiffrement tant
  que l'ADR de transport n'est pas appliquée. Il protège contre l'homonymie et la
  réattribution involontaire, **pas** contre un observateur passif du LAN. C'est
  exactement le périmètre de la décision de transport à venir.
- L'énumération des interfaces est un instantané au démarrage : une interface
  apparue ensuite n'est pas prise en compte sans redémarrage du service. Le
  basculement d'endpoint en cours de session reste à faire.
- Sur une machine sans interface privée LAN routable (conteneur, VPN éteint),
  l'adresse lien local est retenue et signalée comme repli : elle ne traverse aucun
  routeur, les invités distants ne pourront pas l'utiliser.

## Preuves

- `tests/unit/test_main.cpp :: resume_secret_is_the_only_proof_of_a_place` —
  aller-retour 1.2 exact ; champ **obligatoire** (un Hello amputé du secret est
  refusé, pas complété par défaut) ; caractère de contrôle refusé ; `HostAuth` sans
  secret donne un champ vide et une durée nulle ; 200 secrets successifs tous
  distincts, longueur fixe, alphabet hexadécimal minuscule.
- `tests/integration/platform_tests.cpp :: lan_resume_secret_binds_identity` — sur
  TCP réel : homonyme sans secret → autre place, `resumed = 0`, aucun événement de
  reprise ; secret forgé (`0…0`) → autre place ; vrai secret → **même
  `ParticipantId`**, `resumed ≥ 1`, événement « a repris sa place », secret rotaté ;
  départ volontaire → place libérée (état `Revoked`) ; le secret précédent n'a plus
  aucun effet ; aucun secret en clair dans les journaux de l'hôte ou de l'invité.
- `tests/unit/test_main.cpp :: bind_resolution_never_defaults_to_any` — énumération
  réelle (adresses lisibles, boucle locale présente et marquée), sélection LAN sans
  boucle locale ni interface down, adresse non précisée → adresse privée LAN valide
  et note explicite, `any` seulement sur choix explicite signalé.
- Exécution réelle observée sur le poste de travail : interfaces `lo` (127.0.0.1,
  ::1) et `eth0` (169.254.0.21, fe80::fc:ff:fe00:5 %2) ; la résolution retient eth0
  et signale « repli lien local (aucune adresse LAN routable) ».
- Profils ASan+UBSan et `_GLIBCXX_ASSERTIONS` : 0 échec.
