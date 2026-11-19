# Qualification physique DevLink sur LAN

## Statut actuel : `BLOCKED_BY_ENVIRONMENT` — non exécutée

Date de rédaction selon la session : 2026-11-13 UTC. L'environnement accessible
est un conteneur Linux x86_64 sans écran, sans machine invitée et sans backend
WebView2 opérationnel. Il n'a pas deux ordinateurs physiques disponibles. Les
résultats loopback GCC archivés ne qualifient pas le LAN. Les preuves de cette
qualification devront être attachées au commit exact testé ; aucun résultat PASS
n'est revendiqué par ce document.

### Préconditions bloquantes avant tout essai

1. Backend WebView2 réel avec profil isolé, capture vérifiée de la page et entrée
   ciblée ; aujourd'hui le backend échoue fermé.
2. Transport média authentifié et format/encodeur/décodeur reliés au rendu SDL3.
3. Client serveur appliquant les permissions et les modes A/B/C aux paquets d'entrée.
4. Build/doctor/tests Release natifs qualifiés sur les deux machines.
5. Appareils A et B sur un réseau local autorisé, sans désactivation générale du
   pare-feu ni tunnel tiers.

Tant que les quatre premières conditions ne sont pas satisfaites, ne pas simuler un
flux ni marquer les étapes suivantes PASS. Le test de bout en bout ne peut pas
commencer sur l'état courant du produit.

## Runbook requis lorsque les préconditions sont satisfaites

### Matériel et enregistrement non sensible

- **A (hôte)** : Windows x64/UCRT64 qualifié, interface explicitement choisie,
  service web de fixture lié à loopback seulement, DevLink hôte.
- **B (invité)** : ordinateur distinct sur le même LAN, DevLink invité.
- Noter les SHA de commit et des exécutables, versions des OS/toolchains, interfaces
  utilisées, résolution, fréquence écran et heure UTC. Dans le dépôt, ne consigner
  que des adresses de documentation ou les tronquer ; ne consigner aucun code
  d'invitation, secret, cookie, token ou contenu utilisateur.
- Capturer côté A/B les logs expurgés et captures d'écran de la fixture. Garder les
  adresses IP réelles et identifiants d'appareil hors des artifacts publics.

### Scénario

1. Sur A, lancer le service de test et vérifier qu'il n'écoute que sur l'interface
   explicitement prévue. Sélectionner dans DevLink le service et les origines/ports
   exacts ; refuser les origines non sélectionnées.
2. Démarrer la session. L'état `Active` n'est acceptable qu'après navigateur réel,
   première frame capturée et canal authentifié prêts.
3. Sur B, découvrir la session ou saisir manuellement adresse/port. Présenter une
   invitation à usage limité ; l'hôte approuve explicitement l'invité.
4. Vérifier que B reçoit une page qui montre un nonce aléatoire généré pour le run,
   un compteur qui évolue, une zone texte et un bouton. Comparer des marqueurs et
   pixels aux frames de la page sur A ; conserver un hash SHA-256 du payload/capture
   en test, sans stocker de données utilisateur.
5. Tester un clic et une saisie Unicode de B ; démontrer les changements du DOM sur
   A et leur reflet sur B. Enregistrer capture→affichage et entrée→changement DOM.
6. **Mode A** : joindre un second contexte invité indépendant et vérifier que les
   cookies/localStorage définis dans chaque contexte ne sont pas visibles dans
   l'autre.
7. **Mode B** : transférer explicitement le bail ; envoyer des entrées concurrentes
   des deux participants, démontrer que seul le détenteur actuel est accepté, puis
   vérifier journalisation et transfert atomique.
8. **Mode C** : forger hors UI des messages d'entrée valides et invalides ; prouver
   que le serveur les rejette tous sans mutation DOM.
9. Révoquer un invité depuis A ; prouver qu'aucune frame ni entrée ne passe ensuite.
   Arrêter la session ; confirmer fin des sockets/processus WebView2 et profils
   supprimés, ou enregistrer toute suppression différée comme échec/non-clean.
10. Mesurer durant au moins 5 minutes : FPS capture/encode/send/receive/display,
    résolution, débit, latence p50/p95 entrée→DOM, CPU, mémoire, files et pertes.
    Ne publier que les valeurs mesurées et préciser le matériel.

### Résultat et rapport

Pour chaque étape, enregistrer `PASS`, `FAIL`, `NOT RUN` ou `BLOCKED_BY_ENVIRONMENT`
avec commande/interaction exacte, commit, machine, log et preuve. Un test même OS,
loopback, deux processus ou mock reste un test local, pas un LAN physique. Le statut
reste `NOT_FINALIZED` si un critère obligatoire F15 ou les critères produit P0 ne
passent pas.

État de cette exécution : étapes physiques **NOT RUN** ; préconditions produit
**BLOCKED_BY_ENVIRONMENT / NOT_IMPLEMENTED**. Ce fichier est un protocole à exécuter,
pas un rapport de résultat.
