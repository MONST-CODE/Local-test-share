# Notes d'état — DevLink 0.2.0-preview

**Version source :** `engine/core/product.hpp` (`0.2.0-preview`) · **protocole 1.5**
**État :** préversion de développement. Partage réel fonctionnel sous Windows,
**qualifié sur une seule machine** ; aucune release LAN qualifiée.
État détaillé et vérifié : [`docs/reports/IMPLEMENTATION_STATUS.md`](docs/reports/IMPLEMENTATION_STATUS.md).

## Ce qui fonctionne (Windows 10/11 x64, hôte et invité)

- **Moniteur local** : ports TCP/UDP, processus, arbre, terminal probable avec
  niveau de confiance. Détecter n'est jamais autoriser.
- **Partage réel (ADR-018)** : l'application tourne dans un WebView2 hors écran
  sur l'hôte ; l'invité reçoit ses images par un canal TLS 1.3 PSK qui lui est
  propre, et renvoie souris, molette, clavier et texte Unicode. Modes A (profil
  navigateur par invité), B (contrôle exclusif transférable) et C (observation :
  toute entrée refusée par l'hôte). Le navigateur ne joint que les origines
  choisies, via une garde réseau SOCKS5.
- **Plan de contrôle** : admission, défi-réponse HMAC, TLS 1.3 PSK (OpenSSL 3
  épinglé, livré avec l'application), refus de démarrer sans TLS.
- **Messagerie et signalements (ADR-019)** : canal général, messages privés entre
  tous les participants, hôte compris ; signalement d'anomalie de l'invité vers
  l'hôte (gravité, contexte, description), accusé à l'auteur seul ; tout en
  mémoire, oublié à l'arrêt.
- **Poste desktop** SDL3/Dear ImGui, design system DevDark/DevLight, icônes PE.
- **Build** : `build.bat`/`build.sh` directs, dépendances épinglées par SHA-256,
  interface console par étapes `[n/N]` avec bandeau DevLink, journaux complets.

## Non livré ou non qualifié

- **Aucun test LAN sur deux machines physiques** : protocole prêt dans
  `docs/reports/REAL_LAN_QUALIFICATION.md`, pas encore exécuté.
- Pas de mesure sur machine de référence (cinq testeurs, FPS, latence p95).
- Messagerie P1 absente au réseau : indicateur de saisie, accusés de lecture,
  réponses, réactions, mentions, épinglage ; capture jointe aux signalements.
- Viewport fixe 1280×720 (pas de redimensionnement dynamique).
- Pas d'invité ni de navigateur de partage sous Linux ; `build.sh` non exécuté
  sur Linux natif depuis la refonte de son affichage.
- Pas d'adaptateur Docker, pas de mDNS/DNS-SD (découverte multicast propriétaire
  et saisie manuelle).
- `build.bat package` n'a pas été exécuté pour cette version : aucune archive n'est
  publiée dans `releases/`.

## Identité produit

- Nom : DevLink. Version : `0.2.0-preview`.
- Namespace historique `plpt` et marqueur protocolaire `0x504C5054` : identifiants
  internes conservés pour compatibilité, pas l'identité publique.
- La version Windows quadri-partie et les noms d'archive sont dérivés de la source
  produit par les scripts et vérifiés dans les ressources PE.
