# Sécurité DevLink — état réel et risques résiduels

> **Préversion qualifiée sur une seule machine.** Le partage réel (navigateur
> WebView2 sur l'hôte, images chiffrées, entrées autorisées côté hôte) fonctionne
> et est testé en boucle locale sous Windows. Il n'a pas encore été exécuté entre
> deux machines physiques ni soumis à un audit de sécurité indépendant : ne pas
> l'exposer à un réseau non fiable ni l'utiliser pour des données sensibles.
> État vérifié : [`reports/IMPLEMENTATION_STATUS.md`](reports/IMPLEMENTATION_STATUS.md).

## Garanties fournies par le code

- **Pas de transfert du projet** : l'invité reçoit des images JPEG du compositeur,
  jamais le HTML, le JavaScript, les sources ni le `.env`. Le proxy HTTP, qui
  livrerait ces ressources, ne fait plus partie du partage (ADR-013/018).
- **Pas d'accès général à l'hôte** : le navigateur de l'hôte ne joint que les
  origines choisies, par une garde réseau SOCKS5 qui n'ouvre que les couples
  `hôte:port` autorisés (0 connexion mesurée vers un port canari interdit).
  Navigations, redirections, sous-ressources, fenêtres, téléchargements, `file:`,
  `javascript:` et protocoles externes hors allowlist sont refusés. Aucune origine
  `localhost` n'est autorisée implicitement. Le presse-papiers n'est pas relayé.
- **Authentification sans secret en clair** : défi-réponse HMAC-SHA256 lié au
  nonce et à la session (ADR-017), puis TLS 1.3 PSK pour tout le contrôle. TLS 1.2
  est refusé. Sans runtime TLS, le partage **refuse de démarrer** ; aucun repli en
  clair. OpenSSL 3 : bibliothèque système sous Linux, DLL UCRT64 épinglées chargées
  depuis le dossier de l'application sous Windows.
- **Canal média par invité** : clé PSK de 32 octets propre au participant, offerte
  seulement sur le contrôle chiffré ; mauvaise clé, identité usurpée, autre
  session, octets avant TLS et downgrade échouent ; poignées de main limitées.
  Départ, révocation ou réadmission invalident la clé.
- **Autorité côté hôte** : chaque entrée est réautorisée (participant, révocation,
  séquence strictement croissante, suspension, mode, bail). Mode C : toute entrée
  refusée, même d'un client modifié. Mode A : profil navigateur InPrivate par
  invité (le backend de l'application reste commun, ce qui est affiché).
- **Entrées réseau bornées** : longueurs vérifiées avant allocation, UTF-8
  validé, octets surnuméraires refusés. Les signalements (protocole 1.5) ont une
  gravité fermée, une description ≤ 2 048 octets et un contexte sans caractère de
  contrôle (ADR-019).
- **Journaux** : jamais de code d'invitation, de secret TLS, de clé média, de
  frappe ni de texte de message ou de signalement (numéro et gravité seulement).
- **Rétention** : chat, messages privés et signalements en mémoire, bornés, oubliés
  à l'arrêt de la session. L'hôte peut lire les messages privés : pas de
  chiffrement de bout en bout entre invités (§13.3 du maître).

## Limites de qualification

| Domaine | État | Limite principale |
|---|---|---|
| TLS 1.3 PSK, défi-réponse | testé en boucle locale (Windows 2026-11-21, Linux historique) | pas de revue cryptographique indépendante ; pas de confidentialité persistante (PSK) |
| Browser Worker et politique d'origine | testé (`devlink-browser-tests`) | Windows seulement |
| Images, entrées, modes A/B/C | testés, attaques comprises (`devlink-share-tests`, `devlink-desktop-e2e-tests`) | une machine ; jamais sur un vrai LAN |
| Messagerie et signalements | testés sur le contrôle réel (`devlink-platform-tests`) | pas de modération ni de quotas configurables dans l'UI |
| Proxy de service | hors partage | trafic non chiffré : ne pas l'exposer |

## Risques résiduels à traiter

1. **Audit externe** de la dérivation PSK, du lien preuve/session, des nonces,
   expirations, rejeu, rotation et effacement.
2. **LAN physique** : pare-feu, réseaux « publics », isolation Wi-Fi, annonce
   multicast frauduleuse (l'annonce n'authentifie jamais l'hôte), coupures.
3. **Charge** : cinq invités sur la machine de référence, saturation, mémoire.
4. **Cycle de vie** : crash du processus navigateur, arrêt brutal, profils
   orphelins après plantage de l'hôte.
5. **Backend partagé** (mode A) : comptes et données de l'application restent
   communs à tous les invités ; à rappeler aux testeurs.

## Consignes d'utilisation

- Ne pas exposer une session à un réseau non fiable ; autoriser DevLink pour les
  réseaux **privés** seulement.
- N'autoriser que les origines réellement nécessaires à la page.
- Ne pas utiliser le backend synthétique (`--demo-synthetic-browser`) comme preuve
  de partage.
- Ne pas modifier le code pour contourner les gardes TLS ou réseau.
- Ne pas considérer un `SKIP`, un test en boucle locale ou une capture DPI à
  facteur injecté comme une qualification physique.

Une revue interne n'est pas un audit de sécurité indépendant. Une évaluation
externe reste nécessaire avant tout usage sensible ou diffusion générale.
