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
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "../core/clock.hpp"
#include "../core/result.hpp"
#include "../core/secrets.hpp"
#include "../platform/socket.hpp"
#include "transport.hpp"
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
    std::string transport;   // canal réel annoncé par l'hôte pour ce pair
};

struct GuestCounters {
    std::uint64_t frames_received = 0;
    std::uint64_t invalid_frames = 0;
    std::uint64_t chat_received = 0;
    std::uint64_t chat_sent = 0;
    std::uint64_t feedback_sent = 0;
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
    ParticipantId host_participant{}; // publié par l'hôte (1.5) : destinataire privé possible
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
    // Reprise de place : l'indication est publique, le secret ne l'est jamais.
    bool resume_available = false;
    std::uint32_t resume_ttl_ms = 0;
    std::string transport;   // réel : "TLSv1.3 …" ou "tcp clair" (ADR-016)
    // Partage réel (ADR-018) : un canal média a été offert. Le secret n'est
    // jamais dans l'instantané : voir GuestClient::media_offer().
    bool media_offered = false;
    std::uint64_t media_offer_revision = 0;
    std::uint8_t media_mode = 0;
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
    // Secret de reprise obtenu lors d'une admission précédente (voir
    // GuestClient::resume_secret). Vide pour une première connexion.
    std::string resume_secret;
    // ADR-016 / mandat 1355 : refuser un hôte qui ne propose pas l'upgrade TLS.
    bool tls_required = true;
    std::uint32_t tls_handshake_timeout_ms = 5000;
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
    // Signalement d'anomalie vers l'hôte. Refusé localement s'il est vide, trop
    // long ou si la gravité est inconnue ; l'hôte accuse réception par un
    // événement système visible de l'auteur seul.
    bool submit_feedback(FeedbackSeverity severity, std::string description, std::string context);
    bool leave();

    // Secret matériel de reprise, à replacer dans un GuestConfig pour retrouver SA
    // place après une coupure. Ne jamais l'afficher, le journaliser ni l'annoncer :
    // c'est une preuve d'identité, pas une information de session.
    std::string resume_secret() const;
    // Dernière offre de canal média (clé PSK comprise) : à remettre au client
    // média uniquement. Ne jamais l'afficher ni la journaliser.
    std::optional<MediaOffer> media_offer() const;

private:
    enum class CommandKind : std::uint8_t { PostChat, PostDirect, Feedback, Leave, Stop };
    // Type complet : la file de commandes l'instancie dans l'en-tête.
    struct Command {
        CommandKind kind = CommandKind::Stop;
        ParticipantId recipient{};
        std::string text;
        std::string context{};          // Feedback seulement
        std::uint8_t severity = 0;      // Feedback seulement
    };

    bool enqueue(Command command);
    void run(std::stop_token token);
    Result<void> connect_and_authenticate();
    void poll_once(std::chrono::milliseconds timeout);
    void drain_buffered_frames();
    void handle_frame(const model::Envelope& envelope);
    bool queue_frame(model::MessageType type, std::span<const std::uint8_t> payload);
    void flush(std::chrono::milliseconds budget);
    // Basculement transport (ADR-016) : dispatch exclusif du thread d'E/S.
    TransportDescriptor guest_descriptor() const;
    Result<std::size_t> read_some_guest(std::span<std::uint8_t> buffer);
    Result<std::size_t> write_some_guest(std::span<const std::uint8_t> data);
    void close_guest_transport();
    bool upgrade_client(const TlsUpgradeNotice& notice);
    void set_phase(GuestPhase phase, std::string text, ErrorCode code = ErrorCode::Ok);
    void record_event(std::string text);
    void record_warning(Error error);
    void push_chat(ChatLine line);
    void publish();

    IClock& clock_;
    GuestConfig config_;
    Socket socket_;
    std::unique_ptr<ITransportChannel> channel_;   // après upgrade TLS 1.3
    bool secure_ = false;
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
    std::vector<std::uint8_t> challenge_nonce_;   // défi de l'hôte, rejoué par la notice d'upgrade
    std::vector<std::uint8_t> outbox_;
    std::size_t outbox_sent_ = 0;

    GuestSnapshot state_;              // propriété exclusive du thread d'entrées/sorties.
    std::string resume_secret_;        // gardée par mutex_ : lue depuis d'autres threads.
    std::optional<MediaOffer> media_offer_; // gardée par mutex_.
    std::deque<ChatLine> chat_;        // anneau borné, même borne que l'hôte.
    std::deque<std::string> events_;

    mutable std::mutex mutex_;
    std::condition_variable published_;
    std::deque<Command> commands_;
    std::shared_ptr<const GuestSnapshot> view_;
    std::jthread worker_;              // détruit avant les primitives de synchronisation.
};

} // namespace plpt::net
