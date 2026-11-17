# ADR-013 : Formalisation de l'Identité Produit DevLink et Intégration de l'Architecture Complète

## Contexte
Le projet initial portait le nom de code de travail provisoire "PLPT" (Plateforme Locale de Partage et de Test).
Les fondations C++20, la découverte des tables TCP/UDP et l'observateur système ont démontré la viabilité du socle technique.
Conformément au mandat de réalisation de bout en bout du 16 novembre 2026, le produit entre dans sa phase de livraison sous son identité officielle : **DevLink**.
Cette transition requiert :
1. L'adoption du nom officiel DevLink dans les interfaces, dialogues, fenêtres et documentations.
2. L'intégration dans l'application desktop d'une station complète combinant Espace Hôte (découverte, configuration, modération de session, proxy) et Espace Invité (découverte LAN, admission, profil navigateur isolé, chat et feedback).
3. La concrétisation de l'interface `IRemoteBrowserBackend` par `ManagedBrowserBackend` assurant l'isolation stricte des profils et la purge à la fermeture.

## Décision
1. **Identité** :
   - Le nom affiché dans l'interface et la documentation officielle est désormais **DevLink**.
   - La compatibilité avec les binaires et symboles internes existants (`plpt-desktop`, `plpt-monitor`, namespace `plpt`) est préservée afin d'assurer une continuité absolue sans régression des scripts de build ou des tests d'intégration.
2. **Architecture Desktop** :
   - Refonte de `app/desktop_ui/main.cpp` pour orchestrer nativement `HostServer`, `ServiceProxy`, `GuestClient`, `DiscoveryListener`, `Announcer` et `ManagedBrowserBackend`.
   - Fourniture d'une interface graphique native SDL3 / Dear ImGui intégrant le tableau de bord, le centre de découverte, la supervision des sessions, la validation d'invités, la messagerie instantanée, la remontée d'anomalies et les diagnostics réseau.
3. **Moteur d'Isolation de Navigation** :
   - Implémentation de `ManagedBrowserBackend` dans `engine/browser/browser.hpp`.
   - Allocation de répertoires de profil temporaires isolés par contexte de participant.
   - Validation stricte des origines interdisant les accès aux fichiers locaux (`file://`) et aux domaines non autorisés.
   - Purge automatique garantie lors de la destruction des contextes.

## Conséquences et Risques
- **Positives** : Le produit offre désormais l'ensemble des fonctionnalités du scénario directeur attendu dans le document maître d'architecture.
- **Rétrocompatibilité** : Tous les tests unitaires et plateforme existants continuent de passer avec 100% de succès.
- **Sécurité** : Aucun shell, accès disque ou variable secrète n'est exposé. L'isolation des invités est complète.

## Tests et Validation
- 31 cas de tests unitaires (13 835 assertions) : validés sans erreur.
- 7 cas de tests plateforme d'intégration : validés sans erreur.
- Compilation de production et qualification packaging : validées sans erreur.
