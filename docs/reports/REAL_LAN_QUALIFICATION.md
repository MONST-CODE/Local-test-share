# Qualification physique DevLink sur LAN

## Statut actuel : `BLOCKED_BY_ENVIRONMENT` — non exécutée (une seule machine disponible)

Mise à jour : 2026-11-21 (branche `arena/125b0385-local-test-share`). Aucun résultat
LAN n'est revendiqué ici. Deux processus sur une machine, la boucle locale ou un
mock **ne comptent pas**.

### Préconditions produit : désormais satisfaites (sur une machine)

| # | Précondition | État | Preuve |
|---|---|---|---|
| 1 | Navigateur WebView2 réel, profil isolé, capture de vrais pixels, entrée ciblée | satisfaite | `devlink-browser-tests` (identifiant de run relu dans les pixels, clic → image modifiée) |
| 2 | Transport média authentifié relié au rendu SDL3 | satisfaite | `devlink-share-tests` (canal TLS 1.3 PSK par invité), `devlink-desktop-e2e-tests` (vraie fenêtre invitée) |
| 3 | Modes A/B/C imposés côté hôte aux paquets d'entrée | satisfaite | `share_pipeline_tests.cpp` : mode C avec client modifié, mode B avec ancien contrôleur, rejeu |
| 4 | Build et tests Release natifs sur les deux machines | à faire sur B | `build.bat test release` sur A : 17 PASS, 0 SKIP |
| 5 | A et B sur un LAN autorisé, pare-feu non désactivé, sans tunnel | **manquante** | une seule machine disponible |

## Runbook (commandes exactes)

Les deux machines sont des Windows 10/11 x64 avec le runtime Microsoft Edge
WebView2 (présent par défaut sur Windows 11). On utilise le paquet extrait ou
`out\windows\release\bin`.

### Machine A (hôte)

1. Lancer l'application à partager, ou la fixture de qualification :
   `devlink-demo-service.exe --port 0 --run-id <16 hex aléatoires>`. Noter le port
   affiché. Elle n'écoute que sur 127.0.0.1 : l'invité ne peut pas la joindre
   directement, ce qui fait partie du test.
2. Démarrer DevLink en hôte, au choix :
   - **Interface :** Découverte → choisir le service → Configuration (l'origine est
     proposée, vérifier ou corriger) → Mode → « Démarrer le partage » ;
   - **Commande :**
     `devlink-desktop.exe --host http://127.0.0.1:<port> --entry http://127.0.0.1:<port>/fixture --mode B --bind <IP LAN de A> --report-fixture`
     (ajouter `--print-invite` pour afficher le code dans la console de A ; sinon il
     est visible dans l'écran « Session active »).
3. Au premier lancement, Windows demande l'autorisation réseau pour
   `devlink-desktop.exe` : l'accorder au **profil privé** seulement. Ne jamais
   désactiver le pare-feu.

### Machine B (invité)

4. Écran « Sessions du réseau » → « Écouter les annonces » (multicast UDP
   239.255.77.88:47788). À défaut de multicast : saisie manuelle de l'IP et du
   port de contrôle de A.
5. « Rejoindre » avec le code d'invitation, ou en commande :
   `devlink-desktop.exe --join <IP de A> <port de contrôle> --code <code> --name B --request-control`.
6. Vérifier dans « Vue partagée » : FLUX RÉEL, `TLSv1.3 TLS_AES_128_GCM_SHA256`,
   l'identifiant de run de l'étape 1 visible dans la page, l'horloge `t=` qui
   avance.

### Contrôles à enregistrer

7. **Accès direct refusé :** depuis B, `curl http://<IP de A>:<port fixture>/` doit
   échouer. L'application n'est jamais exposée, seules ses images le sont.
8. **Interaction :** cliquer « Cliquer ici » sur B (le carré change de couleur sur B
   **et** `clicks` augmente dans `devlink-host-fixture:` sur A), puis cliquer le
   champ et taper `Unicode é✓` (même valeur dans le DOM de A).
9. **Mode B :** lancer un second invité (B ou une machine C). Donner ou reprendre le
   contrôle depuis « Session active », vérifier que l'ancien contrôleur est refusé
   (colonne « Dernier refus »).
10. **Mode A :** relancer A avec `--mode A`. Les deux invités pilotent chacun leur
    page (deux compteurs indépendants).
11. **Mode C :** relancer A avec `--mode C`. Les entrées de B sont refusées
    (« mode observation seule ») et le DOM reste inchangé.
12. **Révocation et arrêt :** révoquer B depuis A (le flux de B se ferme), puis
    arrêter le partage. Vérifier qu'aucun `msedgewebview2.exe` enfant de DevLink
    ne reste et que le dossier temporaire DevLink a disparu.
13. **Mesures (au moins 5 minutes) :** relever sur B la ligne d'état de la vue
    (img/s, kbit/s, décodage, entrée → image) et, sur A, CPU et mémoire de
    `devlink-desktop.exe` et des processus WebView2. Ne publier que les valeurs
    mesurées, avec le matériel et le Wi-Fi ou Ethernet utilisés.

### Résultat et rapport

Pour chaque étape, noter `PASS`, `FAIL`, `NOT RUN` ou `BLOCKED_BY_ENVIRONMENT`,
avec le commit exact, les machines, la commande et la capture. Ne consigner ni code
d'invitation, ni IP réelle publique, ni cookie. F15 ne passe à `PASS` qu'après les
étapes 4 à 12 exécutées sur deux ordinateurs distincts.

État de cette exécution : étapes physiques **NOT RUN** (`BLOCKED_BY_ENVIRONMENT` :
une seule machine).
