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
#include <span>
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
    std::uint64_t tokens_redacted = 0; // requêtes dont la capacité a été retirée.
    // Un cookie n'est compté « délivré » que si son insertion dans une réponse
    // PERTINENTE (statut >= 200) est effective : une insertion dans un 1xx ne
    // serait pas honorée par le navigateur et ne compte donc pas.
    std::uint64_t grants_issued = 0;   // cookies déposés dans une réponse finale.
    std::uint64_t grants_skipped = 0;  // 101, réponse illisible : jamais modifiée.
    std::uint64_t grants_failed = 0;   // dépôt impossible : en-têtes de réponse hors borne.
    std::uint64_t informational_responses = 0; // 100 Continue, 103 Early Hints…
};

struct ProxyConfig {
    std::string label = "service";
    // Adresse invalide = « PLPT choisit » : une interface privée LAN réellement
    // présente, sinon la boucle locale. Jamais toutes les interfaces par défaut.
    // IpAddress::any() reste possible : c'est alors un choix explicite, affiché.
    IpAddress bind_address{};
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
    // Après autorisation, PLPT dépose la capacité en cookie chez l'invité pour que
    // les sous-ressources de la page passent sans répéter le jeton dans les URL.
    // Le jeton est retiré de chaque requête transmise au service local.
    bool grant_cookie = true;
};

struct ProxySnapshot {
    bool running = false;
    std::uint64_t revision = 0;
    std::string label;
    std::string bind_address;   // adresse réellement liée, jamais celle demandée.
    std::string bind_interface; // interface réelle (« eth0 »), vide si imposée.
    std::string bind_note;      // phrase affichable expliquant le choix retenu.
    bool any_interface = false; // écoute sur toutes les interfaces : à montrer.
    bool loopback_only = false; // aucune interface privée LAN trouvée.
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
    // Interception de la première réponse amont. Les réponses informationnelles
    // (1xx, sauf 101) sont transmises telles quelles et l'interception continue sur
    // les octets suivants : plusieurs 1xx peuvent arriver dans le même segment, la
    // boucle est bornée par les octets réellement reçus. Le cookie de capacité n'est
    // déposé que dans la réponse pertinente (2xx–5xx).
    void process_response_head(Stream& stream, std::span<const std::uint8_t> received);
    // Amont fermé avant une réponse complète : les octets réels sont transmis, le
    // renoncement au cookie est compté et averti, jamais masqué.
    void flush_pending_response_head(Stream& stream, const char* reason);
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
    ResolvedBind bind_{};                              // adresse d'écoute réelle.
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
    bool token_via_cookie = false; // capacité présentée par cookie, pas par URL.
    bool upgrade_websocket = false;
    bool complete = false;        // en-têtes terminés par une ligne vide.
    std::size_t consumed = 0;     // octets d'en-têtes à transmettre tels quels.
};
// Analyse au plus `max_bytes` ; `complete=false` signifie « pas encore fini »,
// jamais « invalide ». Une requête sans ligne de fin dans la borne est refusée.
Result<HandshakeFacts> parse_http_head(std::span<const std::uint8_t> data, std::size_t max_bytes,
    std::string_view token_name);
// Extrait la valeur d'un paramètre de requête ou d'un cookie nommé `name`.
// `via_cookie` reçoit vrai si la valeur provient d'un en-tête Cookie.
std::string extract_token(std::string_view target, std::string_view headers, std::string_view name,
    bool* via_cookie = nullptr);
// Réponse HTTP courte, sans serveur ni date : le proxy n'est pas un service web.
std::vector<std::uint8_t> http_status_response(std::uint16_t status, std::string_view reason, std::string_view body);

// Retire la capacité PLPT d'un en-tête de requête AVANT transmission au service
// local : paramètre « ?name=… » de la cible, cookie « name=… », en-tête
// X-PLPT-Token, et paramètre homonyme dans Referer. Tout le reste est conservé
// octet pour octet ; le résultat n'est jamais plus long que l'entrée.
std::vector<std::uint8_t> redact_capability(std::span<const std::uint8_t> head, std::string_view token_name);
// Code de statut d'une réponse HTTP, 0 si la ligne de statut n'est pas lisible.
// Sert à distinguer une réponse informationnelle (1xx) d'une réponse finale.
int response_status_code(std::span<const std::uint8_t> head);
// Insère une ligne d'en-tête dans une réponse dont la fin d'en-têtes est connue :
// `terminator` est l'index du premier octet de la séquence CRLF CRLF finale.
std::vector<std::uint8_t> inject_response_header(std::span<const std::uint8_t> response, std::size_t terminator,
    std::string_view line);
// Ligne Set-Cookie de capacité : Path=/, HttpOnly, SameSite=Strict, sans Max-Age
// (cookie de session : la capacité meurt avec le navigateur de l'invité).
std::string capability_cookie_line(std::string_view token_name, std::string_view token);

} // namespace plpt::net
