#include "audit.hpp"
namespace plpt {
void AuditLog::record(AuditEvent event, ErrorCode code) {
    if (!enabled_) return;
    if (entries_.size() == 256) entries_.pop_front();
    entries_.push_back({clock_.now_monotonic_ms(), event, code});
}
} // namespace plpt
