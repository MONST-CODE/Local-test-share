#pragma once
// PLPT — engine/net/guest_client.hpp — plan de contrôle côté invité.
// Se connecte à l'hôte, s'authentifie avec le code d'invitation communiqué
// hors bande, reçoit la liste des services réellement publiés (URL + jeton)
// et relaye la messagerie. Un seul thread d'entrées/sorties possède l'état.
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
#include "../core/secrets.hpp"
#include "../platform/socket.hpp"
#include "../session/session.hpp"
#include "chat.hpp"
#include "wire.hpp"

namespace plpt::net {

enum class GuestPhase : std::uint8_t {
    Idle,              // rien n'a été lancé.
    Connecting,        // connexion TCP en cours.
    Authenticating,    // Hello envoyé, code vérifié par l'hôte.
    AwaitingApproval,  // code valide : l'hôte n'a pas encore cliqué « Autoriser ».
    Connected,         // admis : messagerie et services disponibles.
    Refused,           // code refusé, demande refusée, session pleine ou expirée.
    Disconnected,      // connexion perdue après admission.
    Stopped,           // arrêté proprement.
};

inline const char* guest_phase_text(GuestPhase phase) {
    switch (phase) {
    case GuestPhase::Idle: return "inactif";
    case GuestPhase::Connecting: return "connexion";
    case GuestPhase::Authenticating: return "authentification";
    case GuestPhase::AwaitingApproval: return "en attente d'autorisation";
    case GuestPhase::Connected: return "connecté";
    case GuestPhase::Refused: return "refusé";
    case GuestPhase::Disconnected: return "déconnecté";
    case GuestPhase::Stopped: return "arrêté";
    }
    return "inconnu";
}

// Un service tel que l'hôte l'a réellement publié : l'URL contient le jeton
// d'accès, jamais le code d'invitation, et n'est ouverte qu'après admission.
struct GuestService {
    std::uint64_t key = 0;
    std::string label;
    std::uint16_t exposed_port = 0;
    std::string url;
    bool available = true;
    std::uint64_t received_ms = 0;
};

struct GuestPeer {
    ParticipantId id{};
    std::string name;
    model::ParticipantState state = model::ParticipantState::Connected;
    bool self = false;
    std::uint64_t connected_ms = 0;
    std::uint32_t latency_ms = 0;
};

struct GuestCounters {
    std::uint64_t frames_received = 0;
    std::uint64_t invalid_frames = 0;
    std::uint64_t chat_received = 0;
    std::uint64_t chat_sent = 0;
    std::uint64_t errors = 0;
    std::uint64_t reconnects = 0;
};

struct GuestSnapshot {
    GuestPhase phase = GuestPhase::Idle;
    std::uint64_t revision = 0;
    std::string status_text;          // phrase affichable, jamais un secret.
    ErrorCode last_error = ErrorCode::Ok;
    std::uint64_t session_id = 0;
    std::string host_name;
    std::string session_name;
    ParticipantId self{};
    std::string host_address;         // adresse réellement utilisée (celle saisie ou découverte).
    std::uint16_t host_port = 0;
    std::vector<GuestPeer> peers;
    std::vector<GuestService> services;
    std::vector<ChatLine> chat;
    std::vector<std::string> events;
    std::vector<Error> warnings;
    GuestCounters counters;
    std::uint32_t latency_ms = 0;
    std::uint64_t connected_ms = 0;
    bool approval_required = true;
};

struct GuestConfig {
    IpAddress address = IpAddress::loopback(IpFamily::Ipv4);
    std::uint16_t control_port = 0;
    std::string guest_name = "invité";
    InviteCode invite_code;
        std::string client = std::string("PLPT ") + std::to_string(model::kProtocolMajor) + "." + std::to_string(model::kProtocolMinor);
    std::uint32_t connect_timeout_ms = 4000;
    std::uint32_t approval_timeout_ms = 120000;   // attente du clic de l'hôte.
    std::uint32_t inactivity_timeout_ms = 30000;  // au-delà : connexion perdue.
    std::uint32_t heartbeat_interval_ms = 5000;
    bool auto_reconnect = false;                  // jamais sans consentement explicite.
};

class GuestClient {
public:
    explicit GuestClient(IClock& clock) : clock_(clock) {}
    ~GuestClient();
    GuestClient(const GuestClient&) = delete;
    GuestClient& operator=(const GuestClient&) = delete;

    Result<void> start(GuestConfig config);
    void stop();
    bool running() const { return running_.load(); }
    std::shared_ptr<const GuestSnapshot> snapshot() const;
    bool wait_for(const std::function<bool(const GuestSnapshot&)>& predicate, std::chrono::milliseconds timeout);

    bool post_chat(std::string body);
    bool post_direct_message(ParticipantId peer, std::string body);
    bool leave();

private:
    enum class CommandKind : std::uint8_t { PostChat, PostDirect, Leave, Stop };
    // Type complet : la file de commandes l'instancie dans l'en-tête.
    struct Command {
        CommandKind kind = CommandKind::Stop;
        ParticipantId recipient{};
        std::string text;
    };

    bool enqueue(Command command);
    void run(std::stop_token token);
    Result<void> connect_and_authenticate();
    void poll_once(std::chrono::milliseconds timeout);
    void drain_buffered_frames();
    void handle_frame(const model::Envelope& envelope);
    bool queue_frame(model::MessageType type, std::span<const std::uint8_t> payload);
    void flush(std::chrono::milliseconds budget);
    void set_phase(GuestPhase phase, std::string text, ErrorCode code = ErrorCode::Ok);
    void record_event(std::string text);
    void record_warning(Error error);
    void push_chat(ChatLine line);
    void publish();

    IClock& clock_;
    GuestConfig config_;
    Socket socket_;
    WakeupSignal wakeup_;
    std::atomic<bool> running_{false};
    bool connected_ = false;
    std::uint64_t revision_ = 0;
    std::uint64_t published_revision_ = 0;
    std::uint64_t sequence_out_ = 0;   // strictement croissante : jamais de réutilisation.
    std::uint64_t heartbeat_token_ = 1;
    std::uint64_t outstanding_echo_ = 0; // jeton de mesure en vol ; 0 = aucun.
    std::uint64_t heartbeat_sent_ms_ = 0;
    std::uint64_t next_heartbeat_ms_ = 0;
    std::uint64_t last_activity_ms_ = 0;
    std::uint64_t started_ms_ = 0;

    FrameAssembler assembler_;
    std::vector<std::uint8_t> outbox_;
    std::size_t outbox_sent_ = 0;

    GuestSnapshot state_;              // propriété exclusive du thread d'entrées/sorties.
    std::deque<ChatLine> chat_;        // anneau borné, même borne que l'hôte.
    std::deque<std::string> events_;

    mutable std::mutex mutex_;
    std::condition_variable published_;
    std::deque<Command> commands_;
    std::shared_ptr<const GuestSnapshot> view_;
    std::jthread worker_;              // détruit avant les primitives de synchronisation.
};

} // namespace plpt::net
