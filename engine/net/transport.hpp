#pragma once
// DevLink — engine/net/transport.hpp
// Interface de transport VERSIONNÉE (mandat §24) : le plan de contrôle et le plan
// de données ne parlent jamais à un socket directement, ils passent par une
// fabrique qui déclare ce qu'elle est réellement.
//
// Règles absolues (mandat §9.2, §24) :
//   - aucune crypto maison en production : le transport chiffré est TLS 1.3 tel
//     qu'implémenté par OpenSSL (bibliothèque auditée), chargé à l'exécution ;
//   - un transport clair existe MAIS se déclare clair : `peer_authenticated`
//     faux, protocole « tcp clair », et l'interface interdit de le faire passer
//     pour autre chose ;
//   - toute évolution cassante de cette interface incrémente
//     kTransportInterfaceVersion (ADR-016) ; les fabriques la publient, les
//     tests la contrôlent.
//
// Le canal historique `secure_channel` (XOR + HMAC maison) a été SUPPRIMÉ du
// dépôt au basculement du plan de contrôle sur TLS 1.3 (étape 5, ADR-017) :
// plus aucune crypto maison n'est liée, ni en production ni dans les tests.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "../core/result.hpp"

namespace plpt::net {

// Version de l'interface de transport. Incrémenter à chaque changement de
// contrat (méthodes, sémantique d'authentification, politique de versions TLS).
// v2 (basculement étape 5) : adoption d'un descripteur déjà connecté
// (wrap_server/wrap_client) pour l'upgrade TLS en cours de connexion, descripteur
// exposé pour les boucles de poll, et contrat non bloquant explicite
// (WouldBlock) après poignée de main.
inline constexpr std::uint32_t kTransportInterfaceVersion = 2;

// Descripteur natif (fd POSIX, SOCKET Windows) transporté de façon portable.
using TransportDescriptor = std::intptr_t;

// Constantes de protocole TLS (valeurs IANA, pas de magie maison).
inline constexpr std::uint16_t kTls12Version = 0x0303;
inline constexpr std::uint16_t kTls13Version = 0x0304;

enum class TransportKind : std::uint8_t {
    ClearTcp = 1,   // TCP sans chiffrement : déclaré comme tel, jamais implicite
    Tls13Psk = 2,   // TLS 1.3, authentification mutuelle par secret partagé (PSK)
};

inline std::string_view to_string(TransportKind kind) {
    switch (kind) {
    case TransportKind::ClearTcp: return "tcp clair";
    case TransportKind::Tls13Psk: return "tls 1.3 psk";
    }
    return "inconnu";
}

// Ce que le canal EST, pas ce qu'on espère : rempli depuis la poignée de main
// réelle (version négociée lue sur le canal, cipher réellement choisi).
struct TransportInfo {
    TransportKind kind = TransportKind::ClearTcp;
    std::uint32_t interface_version = kTransportInterfaceVersion;
    std::string protocol;          // "TLSv1.3" lu sur le canal, ou "tcp clair"
    std::string cipher;            // cipher TLS réel, vide si transport clair
    bool peer_authenticated = false; // vrai seulement si le pair a prouvé le secret
};

// Canal bidirectionnel octet-flux. La fermeture est idempotente ; aucune copie
// cachée : les spans callers sont respectés.
// Contrat bloquant/non bloquant : send/receive suivent le mode du descripteur
// et renvoient ErrorCode::WouldBlock (code explicite, jamais une attente) quand
// rien n'est disponible sur un descripteur non bloquant. listen/accept/connect
// conservent un descripteur BLOQUANT (tests, clients simples) ; wrap_server/
// wrap_client passent le canal NON BLOQUANT après poignée de main : c'est le
// contrat des boucles de poll du plan de contrôle (basculement étape 5).
class ITransportChannel {
public:
    virtual ~ITransportChannel() = default;
    virtual Result<std::size_t> send(std::span<const std::uint8_t> data) = 0;
    virtual Result<std::size_t> receive(std::span<std::uint8_t> out) = 0;
    virtual void close() = 0;
    virtual TransportInfo info() const = 0;
    // Descripteur sous-jacent, pour poll/epoll/WSAPoll : -1 si fermé.
    virtual TransportDescriptor descriptor() const = 0;
};

// Point d'écoute : une fabrique produit l'endpoint, l'endpoint produit des
// canaux déjà négociés (un canal refusé à la poignée de main n'est JAMAIS
// rendu à l'appelant comme connecté).
class ITransportEndpoint {
public:
    virtual ~ITransportEndpoint() = default;
    virtual Result<std::unique_ptr<ITransportChannel>> accept() = 0;
    virtual std::uint16_t port() const = 0;
    virtual void close() = 0;
};

// Secret hors-bande : code d'invitation ou secret de reprise du plan de
// contrôle. Le PSK TLS 1.3 en est dérivé par hachage standard (SHA-256,
// EVP_Q_digest d'OpenSSL) avec un libellé de domaine versionné ; aucune
// construction de clé maison n'intervient ailleurs.
struct TransportCredentials {
    std::string identity;       // identité PSK affichable, ex. "devlink-session-7"
    std::string shared_secret;  // secret hors-bande (>= 16 caractères exigés)
};

// Politique TLS : par défaut TLS 1.3 strict. Les tests peuvent abaisser le
// maximum pour PROUVER le refus de négociation inférieure ; la production ne
// touche jamais à cette politique.
struct TlsPolicy {
    std::uint16_t min_version = kTls13Version;
    std::uint16_t max_version = kTls13Version;
};

class ITransportFactory {
public:
    virtual ~ITransportFactory() = default;
    virtual std::uint32_t interface_version() const = 0;
    virtual TransportKind kind() const = 0;
    // Disponibilité RÉELLE : une fabrique TLS sans bibliothèque chargée reste
    // construisible mais se déclare indisponible, avec la raison exacte.
    virtual bool available() const = 0;
    virtual std::string_view unavailability_reason() const = 0;
    virtual Result<std::unique_ptr<ITransportEndpoint>> listen(
        const std::string& bind_address, const TransportCredentials& credentials) = 0;
    virtual Result<std::unique_ptr<ITransportChannel>> connect(
        const std::string& address, std::uint16_t port,
        const TransportCredentials& credentials) = 0;
    // Adoption d'une connexion TCP DÉJÀ établie (upgrade en cours de session,
    // étape 5) : la poignée de main se fait sur le descripteur fourni, bornée
    // par handshake_timeout_ms (délais socket bloquants temporaires), puis le
    // canal passe non bloquant. La fabrique prend la propriété du descripteur
    // en cas de succès COMME en cas d'échec (fermé) : jamais de double close.
    virtual Result<std::unique_ptr<ITransportChannel>> wrap_server(
        TransportDescriptor connected, const TransportCredentials& credentials,
        std::uint32_t handshake_timeout_ms) = 0;
    virtual Result<std::unique_ptr<ITransportChannel>> wrap_client(
        TransportDescriptor connected, const TransportCredentials& credentials,
        std::uint32_t handshake_timeout_ms) = 0;
};

// Fabrique TCP clair : diagnostics et tests uniquement. Le plan de contrôle
// production refuse de démarrer un partage avec ce transport (étape 5).
Result<std::unique_ptr<ITransportFactory>> make_clear_tcp_factory();

// Fabrique TLS 1.3 (OpenSSL chargé à l'exécution, PSK mutuel). Si la
// bibliothèque est absente ou incomplète : fabrique construite mais
// available() == false et unavailability_reason() explicite — jamais de
// repli silencieux vers du clair.
Result<std::unique_ptr<ITransportFactory>> make_tls13_factory(
    const TlsPolicy& policy = TlsPolicy{});

// Diagnostic direct pour l'interface et les rapports : raison vide si le
// runtime TLS 1.3 est chargeable ici.
bool tls13_runtime_available(std::string* reason = nullptr);

} // namespace plpt::net
