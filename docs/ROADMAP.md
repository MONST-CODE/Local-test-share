# Roadmap et portes de validation

Le scénario directeur du maître reste prioritaire. Les modèles secondaires ne
justifient pas de contourner une dépendance critique ou un test Windows.

## 0. P0 — architecture de build direct

Seuls `build.bat` et `build.sh` sont autoritaires. La qualification native
Windows/Linux (Debug/Release, tests, mémoire/fuzz, UI, échecs, nettoyage/rebuild et
packages) est décrite dans [le rapport de migration](reports/DIRECT_BUILD_MIGRATION.md).
Cette migration n'ajoute aucune fonction de partage et ne clôt pas les portes
fonctionnelles/humaines ci-dessous.

## 1. Fermer les portes B/C/D sous Windows

- Debug/Release/SDL3 sont construits par MSYS2 UCRT64 clang++ sur runner natif ;
  reproduire sur un poste client avec compte standard, sans élévation.
- Vérifier TCP4/6, UDP4/6, PPID, accès refusés, Unicode, PID recyclés et rapid churn.
- Tester Next.js, Vite, Flask, lancement PowerShell/CMD/IDE et Docker Desktop/WSL2
  (proxy reconnu comme incertain ; pas d'accès Docker ajouté en douce).
- UI : filtrer/sélectionner/protéger, erreurs visibles, fermeture pendant snapshot,
  DPI 100/150/200 %, plusieurs écrans, focus clavier, arrière-plan.
- Installer/désinstaller la preview sur une VM propre ; analyser dépendances et manifeste.

**Porte :** logs Windows natifs et matrice signée par exécution, pas une compilation
croisée. Le moniteur n'est pas bloqué par la recherche sur le navigateur.

## 2. Lot E — prototype WebView2 (risque critique, à mener tôt)

### Avant ajout de la dépendance

Rédiger l'ADR de la version SDK WebView2 et de la stratégie runtime (Evergreen vs
Fixed Version), licences, installation offline, correctifs et coût de distribution.
Aucun numéro de version ou succès de capture n'est inventé dans cette livraison.
Le sandbox courant ne contient ni Windows ni runtime WebView2.

### Matrice expérimentale minimale

| Expérience | Instrumentation / critère |
|---|---|
| Contrôleur de composition + surface dédiée | Mesurer frames réellement capturées, timestamp et dimensions |
| Visible / recouverte / minimisée / hors écran | Continuité et latence, pas seulement une WebView visible dans le dashboard |
| Souris, wheel, key down/up, texte, IME, DPI | Pas de vol d'entrées hôte ; aucune injection clavier/souris système globale |
| Deux profils puis cinq | Cookies/localStorage distincts, HMR/WS, comptes séparés |
| Redirection, popup, iframe, sous-ressource hostile | Refus hors origins confirmées, y compris WS et service workers |
| Téléchargement, file://, clipboard, drag/drop, protocoles externes | Désactivés ou refus explicite ; sandbox conservé |
| Arrêt, crash, fichiers verrouillés | Tous les processus du contexte arrêtés avant nettoyage ; rapport incomplet si échec |

Produire : source prototype isolée, scripts/fixtures, mesures brutes, captures de
la surface seule, protocole de reproduction et rapport d'options. Si capture hors
écran non fiable : arrêter l'intégration, comparer un autre backend ou une fenêtre
de rendu dédiée. Ce compromis nécessite une décision explicite, pas une prétendue
capture off-screen réussie.

**Attention :** une séparation de profils n'isole pas la base Supabase commune.
Les mutations backend restent partagées, à signaler à l'hôte et aux testeurs.

## 3. Lot F — transport et découverte LAN

Choisir une pile QUIC maintenue dans un ADR (Windows d'abord). Vérifier notamment
sa propriété des sockets : port zéro natif, endpoint effectif, listener conservé,
aucun « libérer notre port et rebinder dans la pile ». Un seul endpoint UDP principal ;
TCP fallback uniquement si spécifié et démontré utile. Pas de port par invité.

DNS-SD/mDNS via une implémentation maintenue ou service natif. Tester multicast
5353 occupé, interfaces, réseau invité, firewall et annonce frauduleuse. L'annonce
ne contient aucun secret et n'authentifie jamais l'hôte. Repli manuel IP/port +
invitation forte. Aucun tunnel/service hébergé tiers, aucun contournement réseau.

**Porte :** deux PC Windows authentifiés sur LAN compatible ; ports réellement
réservés ; aucune donnée applicative en clair ; échecs réseau compréhensibles.

## 4. Lot G — autorité de session réelle

- Identité hôte vérifiée et invitations fortes, usage/expiration/révocation.
- Fournisseurs cryptographiques standards ; pas de hash non cryptographique.
- Aucune opération sensible en 0-RTT ; tentative rejouée/expirée refusée.
- Identité de connexion liée au participant, séquences, contextes et capacités.
- Orchestrateur de fermeture : flux, jetons, browser workers, queues, leases,
  profils et annonce ; erreur de nettoyage visible.
- IPC local ACL utilisateur ; admission interne inaccessible à l'invité.

**Porte :** tests négatifs par messages forgés réels, mauvais fingerprint et reprise
après révocation. Les tests de `model::Session` ne suffisent pas.

## 5. Lot H — vrais invités et messagerie

Brancher les modèles via l'orchestrateur, puis compléter présence/typing, réactions,
mentions, épinglage, modération, read receipts et feedback. Les captures ne sont
ajoutées qu'après consentement explicite et restent temporaires. Rétention mémoire
par défaut ; l'hôte est endpoint et peut lire les DMs, pas d'E2EE annoncé.

**Porte :** cinq véritables connexions, contextes distincts, contrôleur partagé unique,
messages correctement routés, révocation indépendante et queues bornées sous charge.

## 6. Lot I — acceptation et release

1. Machine de référence documentée (CPU/GPU/RAM/OS/réseau).
2. Next.js + Supabase/Docker, cinq testeurs sur Wi-Fi réel.
3. Mesurer CPU/GPU/RAM/débit, FPS, drops, p50/p95 latence input->frame, join et chat.
4. Tester pertes, duplication/désordre, coupure Wi-Fi, saturation et crash navigateur.
5. Captures réseau : pas de bundles/source/.env livrés au client.
6. Audit écritures hôte/invité, cache/profils/logs/dumps, nettoyage normal et orphelins.
7. Revue dépendances/CVE/licences, fuzzing prolongé, audit sécurité.
8. Installateur Windows signé, test VM standard et désinstallation ; Linux final ensuite.

Les cibles 30 FPS / 720p, p95 <150 ms et cinq invités sont des **objectifs**, pas
les résultats de ce premier incrément. Les critères §23 du maître doivent tous
être prouvés avant d'appeler le produit « V1 LAN ».

## Hors périmètre de cette livraison

Relais inter-réseaux, backend de données isolé par invité, stockage durable de chat,
export de sources, accès terminal/fichiers/bureau. Aucun de ces chemins n'est ajouté
pour contourner un échec de capture ou de connectivité.
