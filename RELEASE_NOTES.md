# Notes d'état — DevLink 0.2.0-preview

**Version source :** `engine/core/product.hpp` (`0.2.0-preview`)
**État :** préversion de développement, non qualifiée pour le partage réel.

## Présent dans le dépôt

- Poste desktop SDL3/ImGui et moniteur de services locaux.
- Modèles et composants de contrôle hôte/invité, découverte LAN, proxy et
  messagerie, avec tests unitaires et d'intégration en boucle locale.
- Implémentation de transport TLS 1.3 PSK/OpenSSL côté Linux, défi-réponse et
  refus fermé lorsque TLS est requis mais indisponible. Des rapports de tests
  loopback sont conservés dans `docs/reports/evidence/`.
- Branding SVG et exports d'icônes, typographie et ressources de fenêtre.
- Scripts de build directs, contrôles d'archives, ressources ICO/PE et publication
  protégée dans `releases/`.

La présence de ces composants n'implique pas que tous les parcours de produit ou
plateformes soient qualifiés. Les résultats GCC sont exploratoires ; la chaîne
référence Clang++ doit être exécutée séparément.

## Fonctions non livrées

- Le backend WebView2 ne crée pas de contrôleur opérationnel et n'est pas
  disponible comme moteur réel.
- La capture de vrais pixels, l'encodage, le transport média et le rendu chez un
  invité ne sont pas implémentés de bout en bout.
- Les interactions ne sont pas injectées dans un navigateur réel.
- Les modes indépendant, collaboratif et observation ne sont pas validés comme
  parcours réseau complet avec application effective des permissions côté
  serveur.
- La fabrique TLS Windows n'est pas câblée ou qualifiée ; aucune build UCRT64 ni
  exécution native Windows n'est établie dans le présent environnement.
- Aucun test LAN physique à deux machines, qualification de charge multi-invités
  ou DPI physique multi-moniteurs n'est revendiqué.

En conséquence, cette préversion ne doit pas être annoncée comme plateforme de
partage applicatif sécurisée prête à l'emploi. Le code échoue fermé lorsque le
navigateur réel ou le transport requis n'est pas opérationnel.

## Identité produit

- Nom : DevLink.
- Version : `0.2.0-preview`.
- Namespace historique `plpt` et marqueur protocolaire `0x504C5054` : identifiants
  internes conservés pour compatibilité, pas l'identité publique.
- La version Windows quadri-partie et les noms d'archive doivent être dérivés de
  la source produit par les scripts et vérifiés dans les ressources PE.

## Qualification et documentation

Consulter [`docs/reports/FINAL_COMPLETION_AUDIT.md`](docs/reports/FINAL_COMPLETION_AUDIT.md)
pour les statuts par exigence, commandes et points bloquants. Les rapports
historiques gardent leur date et leur environnement d'origine ; ils ne prouvent pas
une exécution fraîche sur la branche courante.
