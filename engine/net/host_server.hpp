#pragma once
// PLPT — engine/net/host_server.hpp — plan de contrôle de la session hôte.
// Un seul thread d'entrées/sorties possède les sockets, la session et la
// messagerie : l'interface ne lit que des instantanés immuables et n'écrit que
// des commandes. Aucune donnée d'interface n'est touchée depuis ce thread.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <thread>
#include <vector>
#include "../core/clock.hpp"
#include "../core/result.hpp"
#include "../core/secrets.hpp"
#include "../messaging/messaging.hpp"
#include "../platform/socket.hpp"
#include "../session/session.hpp"
#include "chat.hpp"
#include "wire.hpp"

namespace plpt::net {

// Libellés français stables pour l'interface et le CLI : une seule source.
inline const char* participant_state_text(model::ParticipantState state) {
    switch (state) {
    case model::ParticipantState::Connected: return "connecté";
    case model::ParticipantState::Idle: return "inactif";
    case model::ParticipantState::Reconnecting: return "reconnexion";
    case model::ParticipantState::Disconnected: return "déconnecté";
    case model::ParticipantState::Revoked: return "révoqué";
    }
    return "inconnu";
}
inline const char* session_state_text(model::SessionState state) {
    switch (state) {
    case model::SessionState::Initializing: return "initialisation";
    case model::SessionState::Discovering: return "découverte";
    case model::SessionState::AwaitingGuests: return "en attente d'invités";
    case model::SessionState::Active: return "active";
    case model::SessionState::Degraded: return "dégradée";
    case model::SessionState::Revoking: return "révocation";
    case model::SessionState::Closing: return "fermeture";
    case model::SessionState::Cleanup: return "nettoyage";
    case model::SessionState::Closed: return "fermée";
    }
    return "inconnu";
}

struct PendingJoin {
    std::uint64_t request_id = 0;
    std::string guest_name;
    std::string address;
    std::uint16_t port = 0;
    std::uint64_t requested_ms = 0;
    std::string client;
};

struct ParticipantRecord {
    ParticipantId id{};
    std::string name;
    std::string address;
    model::ParticipantState state = model::ParticipantState::Connected;
    std::uint64_t connected_ms = 0;
    std::uint64_t last_seen_ms = 0;
    std::uint32_t latency_ms = 0;
    std::uint64_t chat_messages = 0;
    std::string client;
};

// Enregistrement du plan de données : l'application le remplit à partir de
// l'état réel du proxy, puis l'hôte le publie aux invités autorisés.
struct SharedServiceRecord {
    std::uint64_t key = 0;
    std::string label;
    std::uint16_t local_port = 0;   // port de l'hôte, jamais envoyé aux invités.
    std::uint16_t exposed_port = 0; // port réellement joignable sur le LAN.
    std::string token;              // secret : jamais journalisé ni annoncé.
    std::string url;                // ce que l'invité ouvre dans son navigateur.
    bool available = true;
    bool upstream_alive = true;
    std::uint64_t requests = 0;
    std::uint64_t bytes_up = 0;
    std::uint64_t bytes_down = 0;
    std::size_t active_connections = 0;
};

struct HostCounters {
    std::uint64_t connections_total = 0;
    std::uint64_t admitted = 0;
    std::uint64_t denied = 0;
    std::uint64_t revoked = 0;
    std::uint64_t bad_codes = 0;
    std::uint64_t rate_limited = 0;
    std::uint64_t invalid_frames = 0;
    std::uint64_t chat_relayed = 0;
    std::uint64_t timeouts = 0;
};

struct HostSnapshot {
    bool running = false;
    std::uint64_t revision = 0;
    std::uint64_t session_id = 0;
    std::string session_name;
    std::string host_name;
    std::string invite_code;      // affiché à l'hôte seulement, jamais annoncé sur le LAN.
    std::string control_address;  // adresse à dicter à un invité en secours.
    std::uint16_t control_port = 0;
    model::SessionState state = model::SessionState::Initializing;
    std::uint64_t started_ms = 0;
    std::vector<ParticipantRecord> participants;
    std::vector<PendingJoin> pending;
    std::vector<SharedServiceRecord> services;
    std::vector<ChatLine> chat;
    std::vector<std::string> events;
    std::vector<Error> warnings;
    HostCounters counters;
    std::size_t connection_count = 0;
};

struct HostConfig {
    std::string session_name = "Session PLPT";
    std::string host_name = "hôte";
    IpAddress bind_address = IpAddress::any(IpFamily::Ipv4);
    std::uint16_t control_port = 0;              // 0 = port éphémère choisi par l'OS.
    std::string control_address;                 // adresse annoncée aux invités.
    std::size_t max_participants = 5;
    bool approval_required = true;               // l'hôte autorise chaque demande.
    std::uint64_t session_ttl_ms = 4ULL * 60 * 60 * 1000;
    std::uint32_t auth_timeout_ms = 10000;       // sans Hello valide : connexion fermée.
    std::uint32_t approval_timeout_ms = 120000;  // demande en attente oubliée.
    std::uint32_t inactivity_timeout_ms = 30000; // au-delà : participant déconnecté.
    std::uint32_t heartbeat_interval_ms = 5000;
    std::uint32_t reconnect_grace_ms = 30000;    // même code + même nom : même place.
    InviteCode invite_code;
};

class HostServer {
public:
    // Définis dans le .cpp : Connection est incomplet ici, et le constructeur en
    // ligne instancierait la destruction des membres (unique_ptr<Connection>).
    explicit HostServer(IClock& clock);
    ~HostServer();
    HostServer(const HostServer&) = delete;
    HostServer& operator=(const HostServer&) = delete;

    Result<std::uint16_t> start(HostConfig config);
    void stop();
    bool running() const { return running_.load(); }
    std::uint16_t control_port() const { return control_port_; }
    std::uint64_t session_id() const { return session_id_; }
    std::shared_ptr<const HostSnapshot> snapshot() const;

    // Commandes thread-safe : appliquées par le thread d'entrées/sorties.
    bool approve(std::uint64_t request_id);
    bool deny(std::uint64_t request_id, std::string reason);
    bool revoke(ParticipantId participant);
    bool set_services(std::vector<SharedServiceRecord> services);
    bool post_chat(std::string body);
    bool post_direct_message(ParticipantId peer, std::string body);
    bool broadcast_event(std::string text);

    // Attente bornée sur un état réel : tests et interface, jamais de polling actif.
    bool wait_for(const std::function<bool(const HostSnapshot&)>& predicate, std::chrono::milliseconds timeout);
    std::size_t open_connections() const;

private:
    struct Connection;
    enum class CommandKind : std::uint8_t { Approve, Deny, Revoke, SetServices, PostChat, PostDirect, PostEvent, Stop };
    // Une commande d'interface : type complet obligatoire, la file l'instancie.
    struct Command {
        CommandKind kind = CommandKind::Stop;
        std::uint64_t request_id = 0;
        ParticipantId participant{};
        std::string text;
        std::vector<SharedServiceRecord> services;
    };
    // Identité réseau d'un participant : conservée après déconnexion pour que
    // l'hôte voie encore qui était là, et pour la reconnexion dans le délai de grâce.
    struct ParticipantMeta {
        ParticipantId id{};
        std::string name;
        std::string address;
        std::string client;
        std::uint64_t connected_ms = 0;
        std::uint64_t last_seen_ms = 0;
        std::uint32_t latency_ms = 0;
        std::uint64_t chat_messages = 0;
    };
    using DirectKey = std::pair<ParticipantId, ParticipantId>;
    static DirectKey direct_key(ParticipantId left, ParticipantId right) {
        return left < right ? DirectKey{left, right} : DirectKey{right, left};
    }

    bool enqueue(Command command);
    void run(std::stop_token token);
    void drain_commands();
    void accept_pending_connections();
    void service_connections(std::chrono::milliseconds timeout);
    void apply_timeouts();
    // Fermeture réelle : Bye aux pairs admis, purge bornée des files, sockets
    // fermées, puis transitions Closing -> Cleanup -> Closed. Appelée une seule
    // fois, en fin de boucle, y compris si l'arrêt vient du destructeur.
    void shutdown();
    void handle_frame(Connection& connection, const model::Envelope& envelope);
    void handle_hello(Connection& connection, const model::Envelope& envelope);
    void handle_chat(Connection& connection, const model::Envelope& envelope);
    void handle_heartbeat(Connection& connection, const model::Envelope& envelope);
    void handle_bye(Connection& connection, const model::Envelope& envelope);
    void admit(Connection& connection);
    void approve_request(std::uint64_t request_id);
    void deny_request(std::uint64_t request_id, const std::string& reason);
    void revoke_participant(ParticipantId participant);
    void disconnect(Connection& connection, ErrorCode code, std::string text, bool notify_peer);
    // Le protocole interdit participant nul hors Hello/Auth : un pair jamais admis
    // reçoit donc un refus Auth, jamais un Bye qu'il ne pourrait pas décoder.
    bool notify_refusal(Connection& connection, ErrorCode code, const std::string& text);
    bool queue_frame(Connection& connection, model::MessageType type, std::span<const std::uint8_t> payload);
    bool queue_to(ParticipantId participant, model::MessageType type, std::span<const std::uint8_t> payload);
    void broadcast(model::MessageType type, const std::vector<std::uint8_t>& payload, ParticipantId except);
    void broadcast_session_state();
    void broadcast_service_list();
    void relay_chat(const model::ChatMessage& message, const std::string& sender_name, bool direct, ParticipantId only);
    void flush_bounded(std::chrono::milliseconds budget);
    void publish();
    void record_event(std::string text);
    bool rate_limited(const IpAddress& address);
    void record_code_failure(const IpAddress& address);
    SessionInfo build_session_info() const;
    ServiceList build_service_list() const;
    std::vector<ChatLine> build_chat() const;

    IClock& clock_;
    HostConfig config_;
    Socket listener_;
    WakeupSignal wakeup_;
    std::atomic<bool> running_{false};
    std::uint16_t control_port_ = 0;
    std::uint64_t session_id_ = 0;
    std::uint64_t started_ms_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t published_revision_ = 0;
    std::uint64_t next_request_id_ = 1;

    // Propriété exclusive du thread d'entrées/sorties : aucune course possible.
    std::unique_ptr<model::Session> session_;
    std::unique_ptr<model::Messaging> messaging_;
    ParticipantId host_participant_{};
    std::map<ParticipantId, ParticipantMeta> participant_meta_;
    std::map<DirectKey, std::deque<model::ChatMessage>> direct_channels_;
    bool shutdown_done_ = false;     // la fermeture réelle n'a lieu qu'une fois.
    std::uint64_t next_context_ = 1;   // contexte invité unique : jamais celui de l'hôte.
    std::uint64_t heartbeat_token_ = 1;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::vector<SharedServiceRecord> services_;
    std::deque<std::string> events_;
    std::deque<PendingJoin> pending_;
    std::vector<Error> warnings_;
    HostCounters counters_;
    struct AddressAttempt { std::uint64_t window_start_ms = 0; unsigned failures = 0; unsigned connections = 0; };
    std::vector<std::pair<std::string, AddressAttempt>> attempts_;
    struct ReconnectSlot { ParticipantId participant{}; std::string name; std::uint64_t disconnected_ms = 0; };
    std::vector<ReconnectSlot> reconnect_slots_;

    mutable std::mutex mutex_;               // protège commandes, instantané et révision.
    std::condition_variable published_;
    std::deque<Command> commands_;
    std::shared_ptr<const HostSnapshot> view_;
    std::jthread worker_;                    // détruit avant les primitives de synchronisation.
};

} // namespace plpt::net
