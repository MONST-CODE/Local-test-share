#pragma once
#include <cstdint>
#include <deque>
#include "../core/clock.hpp"
#include "../core/result.hpp"
namespace plpt {
enum class AuditEvent : std::uint8_t { SessionCreated, GuestConnected, ParticipantRevoked, RoleChanged, SessionClosed, CleanupIncomplete };
struct AuditEntry {
    std::uint64_t timestamp_ms = 0;
    AuditEvent event = AuditEvent::SessionCreated;
    ErrorCode code = ErrorCode::Ok;
};
// Pas de champ string libre où pourraient aboutir un token, une URL ou un chat.
class AuditLog {
public:
    explicit AuditLog(IClock& clock, bool enabled = false) : clock_(clock), enabled_(enabled) {}
    void record(AuditEvent, ErrorCode = ErrorCode::Ok);
    const std::deque<AuditEntry>& entries() const { return entries_; }
    void clear() { entries_.clear(); }
private:
    IClock& clock_;
    bool enabled_;
    std::deque<AuditEntry> entries_;
};
} // namespace plpt
