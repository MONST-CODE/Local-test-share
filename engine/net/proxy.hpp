#pragma once
// PLPT — engine/net/proxy.hpp — plan de données : pont TCP autorisé par jeton.
// Le proxy écoute sur le LAN, exige le jeton de service comme capacité dans la
// première requête HTTP, puis relie les octets dans les deux sens SANS les
// interpréter : HTTP, SSE et WebSocket (après l'upgrade) passent tels quels.
// Un seul thread d'entrées/sorties possède les sockets et les tampons.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../core/clock.hpp"
#include "../core/result.hpp"
#include "../platform/socket.hpp"

namespace plpt::net {

struct ProxyCounters {
    std::uint64_t connections_total = 0;
    std::uint64_t authorized = 0;
    std::uint64_t refused = 0;          // jeton absent, invalide ou requête illisible.
    std::uint64_t upstream_failures = 0; // service local injoignable : 502 explicite.
    std::uint64_t requests = 0;          // requêtes HTTP observées (1 par flux au moins).
    std::uint64_t websocket_upgrades = 0;
    std::uint64_t bytes_to_guest = 0;
    std::uint64_t bytes_to_upstream = 0;
    std::uint64_t closed = 0;
    std::uint64_t idle_timeouts = 0;
};

struct ProxyConfig {
    std::string label = "service";
    IpAddress bind_address = IpAddress::any(IpFamily::Ipv4); // exposition LAN réelle.
    std::uint16_t exposed_port = 0;                          // 0 = port éphémère.
    IpAddress upstream_address = IpAddress::loopback(IpFamily::Ipv4);
    std::uint16_t upstream_port = 0;                         // obligatoire.
    std::string token;                                       // 32 hex : capacité d'accès.
    std::string token_name = "plpt";                         // nom du paramètre/cookie.
    std::size_t max_connections = 16;
    std::size_t max_buffer_bytes = 256 * 1024;               // par sens et par flux.
    std::uint32_t handshake_timeout_ms = 5000;               // première requête attendue.
    std::uint32_t connect_timeout_ms = 3000;                 // vers le service local.
    std::uint32_t idle_timeout_ms = 120000;                  // flux sans octet.
};

struct ProxySnapshot {
    bool running = false;
    std::uint64_t revision = 0;
    std::string label;
    std::string bind_address;
    std::uint16_t exposed_port = 0;
    std::string url;             // http://<adresse>:<port>/?plpt=<jeton> : à donner à l'invité.
    std::string upstream;        // adresse:port local réellement ponté.
    bool upstream_alive = false; // mesuré par une connexion TCP réelle, jamais supposé.
    std::uint64_t upstream_checked_ms = 0;
    std::size_t active_connections = 0;
    ProxyCounters counters;
    std::vector<std::string> events;
    std::vector<Error> warnings;
};

class ServiceProxy {
public:
    explicit ServiceProxy(IClock& clock);
    ~ServiceProxy();
    ServiceProxy(const ServiceProxy&) = delete;
    ServiceProxy& operator=(const ServiceProxy&) = delete;

    Result<std::uint16_t> start(ProxyConfig config);
    void stop();
    bool running() const { return running_.load(); }
    std::uint16_t exposed_port() const { return exposed_port_; }
    std::shared_ptr<const ProxySnapshot> snapshot() const;
    bool wait_for(const std::function<bool(const ProxySnapshot&)>& predicate, std::chrono::milliseconds timeout);
    // Vérification réelle du service local : ouvre et referme une connexion TCP.
    bool probe_upstream();

private:
    struct Stream;
    enum class CommandKind : std::uint8_t { ProbeUpstream, Stop };
    struct Command { CommandKind kind = CommandKind::Stop; };

    bool enqueue(Command command);
    void run(std::stop_token token);
    void drain_commands();
    void accept_pending();
    void pump_streams(std::chrono::milliseconds timeout);
    void apply_timeouts();
    bool begin_upstream(Stream& stream);
    bool forward(Stream& stream, bool to_guest);
    void close_stream(Stream& stream, const char* reason);
    bool send_status(Stream& stream, std::uint16_t status, std::string_view reason, std::string_view body);
    void publish();
    void record_event(std::string text);
    void record_warning(Error error);

    IClock& clock_;
    ProxyConfig config_;
    Socket listener_;
    WakeupSignal wakeup_;
    std::atomic<bool> running_{false};
    std::uint16_t exposed_port_ = 0;
    std::uint64_t revision_ = 0;
    bool upstream_alive_ = false;
    std::uint64_t upstream_checked_ms_ = 0;
    std::vector<std::unique_ptr<Stream>> streams_;   // propriétaire du thread E/S.
    std::deque<std::string> events_;
    std::vector<Error> warnings_;
    ProxyCounters counters_;

    mutable std::mutex mutex_;
    std::condition_variable published_;
    std::deque<Command> commands_;
    std::shared_ptr<const ProxySnapshot> view_;
    std::jthread worker_;                            // détruit avant mutex_/condition.
};

// Analyse de la première requête HTTP d'un flux, bornée et sans allocation
// superflue. Retourne le jeton présenté (vide si absent) et quelques faits
// utiles à l'audit. Aucune interprétation du corps : le pont reste octet à octet.
struct HandshakeFacts {
    std::string method;
    std::string target;
    std::string version;
    std::string token;
    bool upgrade_websocket = false;
    bool complete = false;        // en-têtes terminés par une ligne vide.
    std::size_t consumed = 0;     // octets d'en-têtes à transmettre tels quels.
};
// Analyse au plus `max_bytes` ; `complete=false` signifie « pas encore fini »,
// jamais « invalide ». Une requête sans ligne de fin dans la borne est refusée.
Result<HandshakeFacts> parse_http_head(std::span<const std::uint8_t> data, std::size_t max_bytes,
    std::string_view token_name);
// Extrait la valeur d'un paramètre de requête ou d'un cookie nommé `name`.
std::string extract_token(std::string_view target, std::string_view headers, std::string_view name);
// Réponse HTTP courte, sans serveur ni date : le proxy n'est pas un service web.
std::vector<std::uint8_t> http_status_response(std::uint16_t status, std::string_view reason, std::string_view body);

} // namespace plpt::net
