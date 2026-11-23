# ADR-019 — Signalements d'anomalie et messages privés vers l'hôte (protocole 1.5)

- **Statut :** accepté, **implémenté** ; testé en boucle locale (Windows x64,
  `devlink-unit-tests` et `devlink-platform-tests`). LAN physique non exécuté.
- **Date :** 2026-11-21
- **Complète :** ADR-008 (messagerie mémoire, hôte endpoint), ADR-017 (canal de
  contrôle TLS 1.3), ADR-018 (protocole 1.4).

## Contexte

Le maître place le signalement d'anomalie en **P0** (FR-MSG-005, §13.2) et donne
le droit de « message privé » à tous les rôles, hôte compris (§4.2). Avant cette
décision :

- le type `MessageType::Feedback` était réservé dans l'enveloppe, la capacité
  `SubmitFeedback` existait dans le modèle de session, mais **aucune route
  réseau** ne transportait un signalement ; l'écran « Signalements » du poste
  desktop le disait explicitement ;
- un invité ne connaissait pas l'identifiant de l'hôte (la liste des pairs
  publiée par l'hôte l'exclut) : il ne pouvait pas lui écrire en privé, alors
  que l'hôte acceptait déjà un tel message côté serveur ;
- l'hôte ne voyait pas ses propres messages privés dans son fil.

## Décisions

### 1. Signalement : invité → hôte uniquement, sur le canal de contrôle chiffré

Nouveau payload `FeedbackReport` (`engine/net/wire.*`), porté par
`MessageType::Feedback` (10) :

| Champ | Type | Borne / validation |
|---|---|---|
| gravité | `u8` | 0 information, 1 mineure, 2 majeure, 3 bloquante ; autre valeur refusée |
| horodatage d'envoi | `u64` | informatif, l'hôte réhorodate à la réception |
| description | texte | UTF-8 valide, 1 à 2 048 octets (`kMaxFeedbackBytes`) |
| contexte | texte | UTF-8 valide, ≤ 256 octets, **aucun caractère de contrôle** |

- Le décodeur vérifie chaque borne **avant** de copier, refuse un octet
  surnuméraire et un payload tronqué.
- L'invité valide localement (description non vide, contexte sans contrôle)
  avant d'émettre ; l'hôte revalide tout : le client n'est jamais cru.
- L'autorisation est celle de la session (`Messaging::report_issue`) : capacité
  `SubmitFeedback` et quotas partagés avec le chat. Tous les rôles invités, y
  compris l'observateur (mode C), peuvent signaler (§4.2).
- L'identifiant du signalement est celui de l'autorité de messagerie
  (`ReportId`), jamais réutilisé dans la session.
- **Accusé de réception** : un événement système « Signalement n°N (gravité …)
  reçu par l'hôte » est envoyé **à l'auteur seul**. Seul cet accusé prouve la
  réception côté invité.
- **Confidentialité et rétention** (§13.3–13.4) : le registre est en mémoire,
  borné à 200 entrées, oublié à l'arrêt de la session. Le journal de l'hôte ne
  contient que le numéro, la gravité et le nom de l'auteur, **jamais le texte**.
- **Pas d'URL capturée automatiquement** : le contexte est un texte saisi par
  l'invité (écran, route, étape). L'interface rappelle de ne pas y coller d'URL
  porteuse de jeton (§13.2). `page_route` du modèle reste vidé à l'ingestion.
- **Hors périmètre** : capture d'écran jointe (FR-MSG-006, P1) et pièces jointes
  (FR-MSG-008 les interdit en V1). Le modèle refuse explicitement une capture.

### 2. Identifiant public de l'hôte dans `SessionInfo`

`SessionInfo` reçoit un bloc optionnel 1.5, **après** le bloc 1.3 : `u64`
identifiant de participant de l'hôte. Ce n'est pas un secret.

- Absent : 0. Présent : non nul, et **jamais égal** à l'identifiant d'un pair
  listé (incohérence refusée au décodage).
- L'invité accepte ce destinataire pour `post_direct_message` ; l'hôte relaie,
  conserve la conversation et renvoie l'écho à l'auteur.

### 3. Messages privés de l'hôte visibles dans son propre fil

Les messages privés émis par l'hôte sont conservés dans le canal privé
correspondant (borné à 200) et apparaissent dans son fil, avec leur destinataire
(`ChatLine::recipient`). L'invité reçoit l'écho de ses propres messages privés,
destinataire compris : il n'affiche que ce que l'hôte a accepté.

### 4. Version 1.5

`kProtocolMinor = 5`. Les mineures restent rétrocompatibles au décodage ; l'hôte
refuse explicitement un invité de mineure inférieure (règle existante depuis 1.2),
pour ne jamais deviner une fonction absente.

## Correction associée

`GuestClient::post_direct_message` lisait `state_.self`, propriété exclusive du
thread d'entrées/sorties, depuis le thread appelant (course de données). La
vérification se fait désormais sur l'instantané publié, sous mutex.

## Conséquences

- L'écran « Signalements » du poste desktop devient fonctionnel : formulaire
  côté invité (gravité, contexte, description, compteur d'octets, accusés),
  registre côté hôte (compteurs par gravité, plus récent en tête).
- La messagerie propose un destinataire : canal général, ou un participant
  publié par le moteur (l'hôte compris pour un invité). Aucun identifiant n'est
  saisi à la main.
- Un invité 1.4 ne peut pas rejoindre un hôte 1.5 (refus explicite).

## Tests

- Unitaires : aller-retour exact de `FeedbackReport`, gravité inconnue, contexte
  avec séquence d'échappement, description surdimensionnée, payload tronqué ;
  bloc 1.5 de `SessionInfo` avec et sans bloc 1.3, hôte listé comme pair,
  octet surnuméraire, bloc tronqué ; négociation de version dérivée de
  `kProtocolMinor`.
- Intégration réseau réelle (`lan_control_plane_host_and_guest`) : refus local
  d'un signalement vide ou à contrôle, réception par l'hôte avec gravité,
  contexte et auteur, compteur, accusé visible de l'auteur seul ; message privé
  invité → hôte, écho à l'auteur, invisibilité pour un tiers ; message privé
  hôte → invité visible de l'hôte.
