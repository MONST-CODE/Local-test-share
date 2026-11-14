#include "session.hpp"
#include <algorithm>
#include <stdexcept>
namespace plpt::model {
inline constexpr std::uint64_t kHostContextValue = ~std::uint64_t{0};
Session::Session(SessionId id, SessionPolicy policy, IClock& clock)
    : id_(id), policy_(policy), clock_(clock), created_ms_(clock.now_monotonic_ms()) {
    if (id.value == 0 || policy.max_participants == 0 || policy.max_participants > 64 || policy.session_ttl_ms == 0)
        throw std::invalid_argument("invalid session policy");
}
bool Session::is_expired() const {
    const auto now = now_ms();
    return now < created_ms_ || now - created_ms_ >= policy_.session_ttl_ms;
}
bool Session::serving() const {
    return !is_expired() && (state_ == SessionState::Active || state_ == SessionState::Degraded || state_ == SessionState::Revoking);
}
Result<void> Session::transition_to(SessionState next) {
    using S = SessionState;
    bool allowed = false;
    switch (state_) {
    case S::Initializing: allowed = next == S::Discovering || next == S::Closing; break;
    case S::Discovering: allowed = next == S::AwaitingGuests || next == S::Closing; break;
    case S::AwaitingGuests: allowed = (next == S::Active && active_count() > 0) || next == S::Closing; break;
    case S::Active: allowed = next == S::Degraded || next == S::Revoking || next == S::Closing; break;
    case S::Degraded: allowed = next == S::Active || next == S::Closing; break;
    case S::Revoking: allowed = next == S::Active || next == S::Closing; break;
    case S::Closing: allowed = next == S::Cleanup; break;
    case S::Cleanup: case S::Closed: break;
    }
    if (next != S::Closing && next != S::Cleanup && is_expired())
        return Result<void>::failure({ErrorCode::Expired, "session expirée", "session"});
    if (!allowed) return Result<void>::failure({ErrorCode::InvalidState, "transition interdite", "session"});
    state_ = next;
    if (next == S::Closing) controllers_.clear();
    return Result<void>::success();
}
Result<void> Session::open_for_guests() {
    if (state_ == SessionState::Active) return Result<void>::success(); // idempotent.
    if (state_ != SessionState::AwaitingGuests)
        return Result<void>::failure({ErrorCode::InvalidState, "ouverture interdite depuis cet état", "session"});
    if (is_expired()) return Result<void>::failure({ErrorCode::Expired, "session expirée", "session"});
    state_ = SessionState::Active;
    return Result<void>::success();
}
Result<void> Session::complete_cleanup(const CleanupReport& report) {
    if (state_ != SessionState::Cleanup) return Result<void>::failure({ErrorCode::InvalidState, "pas en nettoyage", "session"});
    if (!report.complete || !report.failures.empty())
        return Result<void>::failure({ErrorCode::IoError, "nettoyage incomplet", "session"});
    participants_.clear(); shared_contexts_.clear(); controllers_.clear(); host_.reset();
    state_ = SessionState::Closed;
    return Result<void>::success();
}
std::size_t Session::active_count() const {
    return static_cast<std::size_t>(std::count_if(participants_.begin(), participants_.end(), [](const auto& item) {
        return item.second.state != ParticipantState::Revoked && item.second.role != Role::Host;
    })); // un invité déconnecté réserve sa place jusqu'à révocation/expiration.
}
Result<ParticipantId> Session::register_host() {
    if (host_) return Result<ParticipantId>::success(*host_); // idempotent : un seul hôte.
    if (state_ == SessionState::Closing || state_ == SessionState::Cleanup || state_ == SessionState::Closed)
        return Result<ParticipantId>::failure({ErrorCode::InvalidState, "session en fermeture", "session"});
    const ParticipantId pid{next_id_++};
    // Contexte réservé, jamais attribuable à un invité : aucun invité ne peut
    // prétendre partager le contexte de l'hôte.
    participants_.emplace(pid, Participant{pid, Role::Host, ParticipantState::Connected, ContextId{kHostContextValue}});
    host_ = pid;
    return Result<ParticipantId>::success(pid);
}
Result<ParticipantId> Session::admit_verified_guest(Role role, ContextId context) {
    if (is_expired()) return Result<ParticipantId>::failure({ErrorCode::Expired, "session expirée", "session"});
    if (state_ != SessionState::AwaitingGuests && state_ != SessionState::Active && state_ != SessionState::Degraded)
        return Result<ParticipantId>::failure({ErrorCode::InvalidState, "admission fermée", "session"});
    if (role == Role::Host || role > Role::Observer || context.value == 0)
        return Result<ParticipantId>::failure({ErrorCode::PermissionDenied, "rôle/contexte invité invalide", "session"});
    if (active_count() >= policy_.max_participants || participants_.size() >= 128)
        return Result<ParticipantId>::failure({ErrorCode::CapacityExceeded, "capacité atteinte", "session"});
    const bool shared = role != Role::IndependentTester;
    const auto it = shared_contexts_.find(context);
    if (it != shared_contexts_.end() && (!shared || !it->second))
        return Result<ParticipantId>::failure({ErrorCode::PermissionDenied, "contexte indépendant réservé", "session"});
    shared_contexts_[context] = shared;
    const ParticipantId pid{next_id_++};
    participants_.emplace(pid, Participant{pid, role, ParticipantState::Connected, context});
    if (state_ == SessionState::AwaitingGuests) state_ = SessionState::Active;
    return Result<ParticipantId>::success(pid);
}
const Participant* Session::find_participant(ParticipantId pid) const {
    const auto it = participants_.find(pid);
    return it == participants_.end() ? nullptr : &it->second;
}
Result<void> Session::set_participant_state(ParticipantId pid, ParticipantState next) {
    if (!serving()) return Result<void>::failure({ErrorCode::InvalidState, "session inactive", "session"});
    const auto it = participants_.find(pid);
    if (it == participants_.end()) return Result<void>::failure({ErrorCode::NotFound, "participant inconnu", "session"});
    if (next > ParticipantState::Revoked || it->second.state == ParticipantState::Revoked)
        return Result<void>::failure({ErrorCode::PermissionDenied, "état interdit", "session"});
    it->second.state = next;
    if (next != ParticipantState::Connected && next != ParticipantState::Idle) {
        const auto controller = controllers_.find(it->second.browser_context);
        if (controller != controllers_.end() && controller->second == pid) controllers_.erase(controller);
    }
    return Result<void>::success();
}
Result<void> Session::revoke_participant(ParticipantId pid) {
    const auto* p = find_participant(pid);
    if (p && p->state == ParticipantState::Revoked) return Result<void>::success(); // idempotent.
    return set_participant_state(pid, ParticipantState::Revoked);
}
bool Session::can(ParticipantId pid, Capability cap) const {
    const auto* p = find_participant(pid);
    if (!serving() || !p || (p->state != ParticipantState::Connected && p->state != ParticipantState::Idle)) return false;
    if (!role_has_capability(p->role, cap)) return false;
    if (cap == Capability::SendInput && p->role == Role::Collaborator) {
        const auto it = controllers_.find(p->browser_context);
        return it != controllers_.end() && it->second == pid;
    }
    return true;
}
bool Session::can_input(ParticipantId pid, ContextId target) const {
    const auto* p = find_participant(pid);
    return p && p->browser_context == target && can(pid, Capability::SendInput);
}
Result<void> Session::grant_control(ParticipantId pid) {
    const auto* p = find_participant(pid);
    if (!p || p->role != Role::Collaborator || !can(pid, Capability::RequestControl))
        return Result<void>::failure({ErrorCode::PermissionDenied, "contrôle interdit", "session"});
    controllers_[p->browser_context] = pid; // remplacement atomique sur le thread moteur.
    return Result<void>::success();
}
} // namespace plpt::model
