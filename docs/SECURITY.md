# Sécurité — garanties limitées à l'incrément livré

**Cette preview ne partage aucune application.** Le transport authentifié,
WebView2, la politique de navigation et la suppression des profils ne sont pas
implémentés. Ne pas l'exposer comme une version LAN utilisable par des invités.

## Frontières actuelles

- Les adaptateurs observent le système local sous le compte courant. Aucune élévation,
  injection, commande shell, collecte d'environnement, `.env` ou historique de terminal.
- La sélection d'un listener TCP est un état local ; elle ne publie rien et ne
  configure pas le pare-feu. Il n'existe aucun bouton d'invitation opérationnel.
- Les exécutables n'utilisent pas `DynamicPortAllocator`. Ses sockets sont ouverts
  seulement dans les tests/prototypes explicitement lancés.
- Les modèles session/chat/protocole ne sont pas liés au moniteur distribué.
- Une origine probable, une IP, un PID ou une annonce mDNS ne sera jamais une identité.

## Ce qui est vérifié vs requis avant partage

| Menace | Contrôle présent | Porte restante |
|---|---|---|
| PID recyclé / table partielle | Création avant/après, sélection invalidée, états Gone/Unavailable ; tests | Churn natif Windows avec Next/Vite/Docker/WSL |
| Usurpation de port | Socket conservé, exclusivité OS, handle RAII ; tests loopback Linux | Exécution Winsock x64, pare-feu, pile QUIC réelle |
| Observateur envoie une entrée | Refus dans `model::Session`, contexte et contrôleur exclusif | Relier chaque message authentifié au moteur, tests de paquets forgés |
| DM divulgué dans le canal général | Contrôles acteur/destinataire, réponses privées, quotas ; tests | Routage réseau authentifié, reprise, modération |
| Message énorme ou invalide | Codec borné avant allocation ; UTF-8 et quotas dans le modèle ; fuzz smoke | Parser incrémental QUIC, backpressure, fuzz long et charge |
| Invitation volée/rejouée | **Aucun système d'invitation livré** ; ancien FNV retiré | CSPRNG, empreinte fournisseur audité, fingerprint hôte, expiration, usages, quotas |
| Écoute LAN / faux mDNS | Aucun canal PLPT ni annonce dans cette preview | QUIC/TLS, identité hôte, invitation validée, refus 0-RTT sensible |
| SSRF / accès aux fichiers depuis la page | Aucun navigateur exécuté ; ce n'est pas un filtre testé | Origines strictes, redirections, sous-ressources, WS, service workers, protocoles externes |
| Suppression d'un dossier non possédé | Backend de stockage **indisponible**, donc aucune suppression | Leases, ACL, reparse points, fin des processus, test crash |
| Fuite par logs applicatifs | Audit typé sans texte libre, borné, désactivé par défaut ; pas d'INI/log ImGui | Audit OS/driver/profil/dumps sur les vraies machines |

## Cryptographie et invitations

Pas d'algorithme maison, de secret « haché » en FNV, de pseudo-TLS ou de jeton
comparé dans un simple transport en mémoire. BCryptGenRandom / getrandom sont
les seules primitives d'entropie utilisées, sans fallback faible. Un appel RNG
réussi n'est pas une validation du handshake, qui n'existe pas encore.

Le choix QUIC/TLS doit être un ADR séparé : version, licence, canal de correction,
API d'identité et de certificats, disponibilité Windows/Linux, ownership du socket.
Les opérations rejouables (consommation d'invitation, rôle, contrôle) doivent attendre
l'authentification finale ; pas de traitement sensible en 0-RTT. La découverte
mDNS future reste strictement non authentifiante.

## Autorisation future

L'identité d'une connexion vient du transport authentifié, **pas du champ
participant_id fourni par l'invité**. L'orchestrateur doit vérifier, dans cet ordre :

1. état de connexion et exclusion de données précoces ;
2. session, expiration, révocation ;
3. type, limites et version négociée ;
4. identité enveloppe = identité liée à la connexion ;
5. séquence / canal / capacité ;
6. contexte autorisé et verrou de contrôle ;
7. quota et file bornée ;
8. seulement ensuite : effet dans le Browser Worker ou la messagerie.

Le codec livré effectue une partie de l'étape 3, pas ces huit étapes. Aucun accès
à une méthode C++ interne ne doit devenir automatiquement une commande réseau.

## Politique navigateur à démontrer au Lot E

L'allowlist devra couvrir schéma, hôte normalisé, port et origines confirmées, pas
une comparaison de préfixe de chaîne. Tester `file:`, `ftp:`, protocoles externes,
popups, iframes, redirections, sous-ressources, WS/HMR, service workers, permissions,
clipboard, drag/drop et téléchargements. DNS rebinding, metadata cloud et interfaces
admin locales doivent être considérés. Tester la capacité réelle de WebView2 à
intercepter **tous** ces chemins ; une navigation top-level filtrée ne suffit pas.

Garder le sandbox du moteur Web et ses mises à jour. Ne pas capturer le bureau
complet. Ne pas utiliser un mécanisme d'injection système de clavier/souris pour
contourner les difficultés d'entrée dans la WebView. Le prototype doit échouer
explicitement lorsqu'une garantie n'est pas démontrable.

## Persistance et nettoyage

La preview n'a ni profil navigateur ni fichier de chat. Cela ne permet **pas**
d'affirmer « aucune donnée écrite » : l'OS, SDL/drivers, le terminal, des dumps ou
le swap peuvent laisser des traces. La capture `--screenshot` de smoke test écrit
un BMP uniquement à la demande explicite du développeur. Les fixtures de tests
écrivent leurs propres fichiers temporaires ; elles ne sont pas une implémentation
de stockage de session.

La libération de collections C++ ne garantit pas l'écrasement physique de leurs
allocations. SSD, journaling et pagefile rendent l'effacement absolu non garanti.
Un besoin strict RAM-only/chiffrement temporaire exige un prototype et un ADR.

L'installateur proposé est par utilisateur, sans service, auto-démarrage, règle
pare-feu ni élévation. Son uninstall supprime des noms de fichiers connus, jamais
une racine arbitraire en récursif. Installation, désinstallation, signature et
SmartScreen **restent à tester sous Windows**.

## Critères de refus de release

Une V1 ne peut être annoncée avant les critères du maître §23, les tests natifs
Windows, l'audit des frontières navigateur/transport, le nettoyage normal/après
crash et les mesures avec cinq véritables clients. Les résultats de tests de ce
dépôt n'équivalent pas à un audit de sécurité indépendant.
