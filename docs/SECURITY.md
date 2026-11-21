# Sécurité DevLink — état réel et risques résiduels

> **Cette branche n'est pas une release de partage d'applications.** Le vrai
> Browser Worker WebView2, la capture, le streaming des pixels, l'injection
> d'interactions et l'allowlist appliquée au navigateur ne sont pas opérationnels.
> Le fail-closed doit rester activé : ne pas ouvrir une session de partage réelle
> en production.

## Ce qui existe dans le code

- Génération de secrets via les primitives OS existantes et messages de contrôle
  bornés/validés.
- Hôte/invité avec défi-réponse, admission et révocation dans le moteur ; les
  parcours réseau ont des tests loopback.
- Transport TLS 1.3 PSK utilisant OpenSSL chargé à l'exécution sur Linux. La
  politique par défaut interdit les versions TLS inférieures ; un mauvais secret
  ou l'absence du runtime doit échouer sans repli silencieux.
- Le backend WebView2 est explicitement indisponible. Le backend géré synthétique
  est réservé aux tests et démos identifiés ; il ne doit pas être décrit comme un
  navigateur réel.
- Le proxy applicatif existe, mais son transport de données n'est pas établi comme
  chiffré de bout en bout.

## Limites de qualification

Les preuves d'intégration TLS consignées sont des tests loopback Linux. Elles ne
constituent ni un audit indépendant, ni un test sur deux machines physiques, ni une
qualification Windows. Le code TLS Windows est indisponible dans l'état courant.
La disponibilité d'OpenSSL sous Linux ne démontre pas la disponibilité de WebView2,
qui est propre à Windows.

La matrice de l'audit doit distinguer les composants :

| Domaine | État honnête | Limite principale |
|---|---|---|
| TLS 1.3 PSK côté Linux | `IMPLEMENTED_BUT_NOT_VERIFIED` pour livraison courante | rapports loopback présents, build de référence non rejoué ici |
| TLS Windows/UCRT64 | `BLOCKED` | machine/runtime Windows absent et backend non câblé |
| Authentification et défi-réponse | `PARTIALLY_IMPLEMENTED` | tests locaux présents, pas d'E2E physique ni revue indépendante |
| Proxy de service | `PARTIALLY_IMPLEMENTED` | données HTTP/proxy non qualifiées chiffrées de bout en bout |
| Browser Worker et politique d'origine | `NOT_IMPLEMENTED` | pas de contrôleur WebView2 réel ni interception des navigations |
| Frames et interactions | `NOT_IMPLEMENTED` | aucun pipeline réel, aucun format wire média validé |
| Modes A/B/C sur réseau | `PARTIALLY_IMPLEMENTED` | modèles/tests locaux ne démontrent pas l'application E2E des capacités |

## Modèle de menace visé

L'invité ne doit jamais recevoir par défaut un shell, le bureau, le système de
fichiers, le dépôt source, les fichiers `.env`, les ports locaux arbitraires, Docker
ou une console d'administration. La découverte d'un port n'est jamais une
autorisation. Le produit final devra authentifier chaque connexion, limiter les
capacités à une session/participant, appliquer les permissions côté moteur et
révoquer ces capacités immédiatement.

Ces objectifs ne doivent pas être confondus avec des garanties actuellement
fournies par le pipeline navigateur, qui manque encore.

## Risques techniques à traiter avant le partage réel

1. **TLS/identité :** auditer la dérivation PSK, le lien preuve/session, les
   nonces, expirations, rejeu, rotation et effacement. La revue actuelle est
   interne uniquement ; le PSK ne fournit pas de confidentialité persistante
   contre la compromission ultérieure du secret.
2. **Windows :** brancher une pile TLS maintenue et tester identité, versions,
   échecs et absence de clair sous UCRT64 ; aucun badge « sécurisé » avant cela.
3. **Proxy :** établir le chiffrement du canal de données, restreindre destinations,
   ports, redirections et origines ; tester SSRF/rebinding et la révocation.
4. **WebView2 :** filtrer URL normalisée (schéma/hôte/port), redirections,
   sous-ressources, fenêtres, téléchargements, `file:`, protocoles externes et
   permissions. Pas d'accès système aux entrées invité.
5. **Média :** un codec payload v1 big-endian borné et une file raw-frame limitée
   existent (`remote_frame.hpp`, `engine/media/frame_queue.hpp`), mais ils ne sont
   pas raccordés au transport TLS, à WebView2, au décodage compressé ou au rendu.
   Il reste à tester le pipeline end-to-end, le backpressure réseau et le décodage
   contre des entrées adversariales.
6. **Modes et capacités :** vérifier côté serveur toute entrée, bail exclusif du
   mode collaboratif et refus absolu des événements pour le mode observation.
7. **Cycle de vie :** tests de démarrage partiel, panne, révocation, crash, arrêt,
   destruction de callbacks/profils et absence de processus navigateur orphelins.

## Consignes d'utilisation

- Ne pas exposer cette branche à un réseau non fiable.
- Ne pas utiliser le backend synthétique comme preuve de partage.
- Ne pas modifier le code pour contourner les gardes TLS/WebView2.
- Ne pas journaliser les codes d'invitation, secrets TLS, cookies, jetons de capacité
  ou contenu utilisateur.
- Ne pas considérer un test `SKIP`, GCC exploratoire, boucle locale ou capture DPI
  à facteur injecté comme une qualification physique.

Une revue interne n'est pas un audit de sécurité indépendant. Une évaluation de
sécurité externe reste nécessaire avant tout usage sensible ou diffusion générale.
