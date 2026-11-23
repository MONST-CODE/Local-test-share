# Guide d'utilisation — partager une application web locale

> **Portée réelle :** hôte et invité sous Windows 10/11 x64 (runtime Microsoft Edge
> WebView2 requis côté hôte, présent par défaut sur Windows 11). Le parcours est
> qualifié sur **une machine** (vrais exécutables hôte et invité). Le test sur deux
> ordinateurs du même LAN n'a pas encore été exécuté : voir
> `docs/reports/REAL_LAN_QUALIFICATION.md`.

## Ce que voit et fait l'invité

L'invité **ne reçoit ni le code, ni les fichiers, ni le `.env`, ni un accès HTTP** à
votre application. Un navigateur s'exécute sur votre machine, l'invité reçoit ses
images et lui renvoie souris, molette, clavier et texte. Il ne peut joindre que les
origines que vous avez choisies : pas d'autre port local, pas de base de données,
pas de Docker, pas de `file://`.

## Côté hôte

1. Lancez votre application comme d'habitude (Next.js, API, base locale…).
2. **Découverte** : repérez le serveur de l'application (port, processus) et
   cliquez « Choisir ». Rien n'est partagé à cette étape.
3. **Configuration** :
   - vérifiez l'**origine autorisée** proposée (par exemple
     `http://127.0.0.1:3000`). Ajoutez, séparées par des virgules, les autres
     origines dont la page a besoin (par exemple une API sur un autre port) ;
   - l'**URL d'entrée** (vide = racine de la première origine) ;
   - le **mode** :
     - **A — test indépendant** : chaque invité a son propre navigateur (cookies et
       stockage séparés). Le backend de l'application reste commun : un compte créé
       par un invité existe pour les autres ;
     - **B — collaboration** : une vue commune, un seul invité pilote à la fois ;
       vous donnez ou reprenez le contrôle ;
     - **C — observation** : les invités regardent, toute entrée est refusée par
       votre machine ;
   - l'approbation de chaque invité (activée par défaut).
4. « **Démarrer le partage** ». L'état passe par les étapes réelles (navigateur,
   chargement, première image, canal média). « Actif » n'apparaît qu'une fois tout
   confirmé. Un échec est affiché avec sa cause.
5. Communiquez le **code d'invitation** (écran « Session active ») par un canal de
   votre choix. Il n'est jamais diffusé sur le réseau.
6. **Session active** : approuvez, révoquez, donnez ou reprenez le contrôle,
   suspendez les entrées, suivez images et entrées acceptées ou refusées par invité.
   « Vue partagée » montre ce que voient les invités.
7. « **Arrêter la session** » coupe les flux, détruit les navigateurs et oublie les
   secrets.

Au premier partage, Windows demande l'accès réseau : autorisez-le pour les
**réseaux privés** uniquement.

## Côté invité

1. Basculez sur « Espace invité ». Dans **Sessions du réseau**, écoutez les
   annonces ou saisissez l'adresse et le port de l'hôte.
2. **Rejoindre** : votre nom et le code d'invitation.
3. Une fois admis, la **Vue partagée** s'ouvre : FLUX RÉEL, TLS 1.3, mode et droits
   s'affichent tels que l'hôte les décide. En mode B, « Demander le contrôle ».
4. Cliquez dans la page pour la piloter : le clavier lui est alors envoyé. Cliquez
   hors de la vue pour le récupérer. Le presse-papier n'est pas partagé.

## Si cela ne marche pas

| Symptôme | Cause probable |
|---|---|
| « Moteur navigateur indisponible » | runtime WebView2 absent sur l'hôte |
| « TLS 1.3 indisponible » | DLL OpenSSL du paquet manquantes : réinstallez le paquet complet |
| Invité : « hôte injoignable sur le port média (pare-feu ?) » | pare-feu Windows de l'hôte, ou réseau « public » |
| Aucune session annoncée | multicast bloqué par le réseau : saisie manuelle de l'adresse et du port |
| Page blanche ou bloquée | une ressource vient d'une origine non autorisée : ajoutez-la (compteurs « bloqués » dans Session active) |

## Messagerie et signalements

Les deux passent par le canal de contrôle chiffré (TLS 1.3) de la session, sans
service tiers. Rien n'est conservé après l'arrêt de la session.

**Messagerie** (onglet Messagerie) : choisissez le destinataire à gauche de la
zone de saisie, « Canal général » ou un participant (un invité peut aussi écrire
à l'hôte). Un message privé est signalé « (privé) A → B ». L'hôte étant le serveur
de la session, il peut lire les messages privés entre invités : ils ne sont pas
chiffrés de bout en bout entre invités.

**Signalements** (onglet Signalements) :

- *Côté invité* : choisissez la gravité (information, mineure, majeure, bloquante),
  indiquez éventuellement le contexte (écran, route, étape) et décrivez ce que vous
  avez fait, ce que vous attendiez et ce qui s'est produit (2 048 octets au plus).
  « Envoyer le signalement ». L'accusé de réception de l'hôte apparaît sous le
  formulaire : seul cet accusé prouve la réception. N'y collez pas d'URL contenant
  un jeton, un identifiant ou une donnée personnelle. Aucune capture d'écran ni
  pièce jointe n'est envoyée.
- *Côté hôte* : les signalements arrivent en tête de liste, avec gravité, auteur,
  contexte et description, et des compteurs par gravité. Le journal de la session
  ne garde que le numéro et la gravité, jamais le texte.
