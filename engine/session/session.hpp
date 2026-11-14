#pragma once
// Modèle LOCAL expérimental. Aucun handshake, invitation ou accès réseau ici.
// Les commandes sont réservées au moteur de confiance, jamais à un payload invité.
#include <map>
#include <optional>
#include "../core/clock.hpp"
#include "../platform/platform.hpp"

namespace plpt::model {
enum class SessionState : std::uint8_t {
    Initializing, Discovering, AwaitingGuests, Active, Degraded, Revoking, Closing, Cleanup, Closed
};
enum class ParticipantState : std::uint8_t { Connected, Idle, Reconnecting, Disconnected, Revoked };
struct Participant {
    ParticipantId id{};
    Role role = Role::Observer;
    ParticipantState state = ParticipantState::Connected;
    ContextId browser_context{};
};
struct SessionPolicy {
    std::size_t max_participants = 5;
    std::uint64_t session_ttl_ms = 4 * 60 * 60 * 1000;
};
class Session {
public:
    Session(SessionId, SessionPolicy, IClock&);
    SessionState state() const { return state_; }
    SessionId id() const { return id_; }
    std::uint64_t now_ms() const { return clock_.now_monotonic_ms(); }
    bool is_expired() const;
    Result<void> transition_to(SessionState);
    // Une session publiée et en attente d'invités est déjà active : l'hôte peut y
    // écrire et autoriser des demandes. transition_to() garde ses règles strictes.
    Result<void> open_for_guests();
    Result<void> complete_cleanup(const CleanupReport&); // un échec laisse l'état Cleanup.
    // L'hôte local est un participant du canal de messagerie, mais il n'occupe
    // jamais une place d'invité : active_count() ne compte que les invités admis.
    Result<ParticipantId> register_host();
    std::optional<ParticipantId> host_participant() const { return host_; }
    // Appelée uniquement après vérification du code d'invitation et, si elle est
    // exigée, approbation explicite de l'hôte. Jamais depuis un payload invité.
    Result<ParticipantId> admit_verified_guest(Role, ContextId);
    Result<void> set_participant_state(ParticipantId, ParticipantState);
    Result<void> revoke_participant(ParticipantId);
    Result<void> grant_control(ParticipantId); // décision de l'hôte pour une vue collaborative.
    const Participant* find_participant(ParticipantId) const;
    bool can(ParticipantId, Capability) const;
    bool can_input(ParticipantId, ContextId target) const;
    std::size_t active_count() const;
private:
    bool serving() const;
    SessionId id_;
    SessionPolicy policy_;
    IClock& clock_;
    std::uint64_t created_ms_;
    SessionState state_ = SessionState::Initializing;
    std::map<ParticipantId, Participant> participants_;
    std::map<ContextId, bool> shared_contexts_;
    std::map<ContextId, ParticipantId> controllers_;
    std::optional<ParticipantId> host_;
    std::uint64_t next_id_ = 1;
};
} // namespace plpt::model
