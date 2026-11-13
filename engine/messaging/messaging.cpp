#include "messaging.hpp"
#include "../core/text.hpp"
#include <algorithm>
namespace plpt::model {
namespace {
Error denied() { return {ErrorCode::PermissionDenied, "accès au canal refusé", "messaging"}; }
}
Messaging::DmKey Messaging::dm_key(ParticipantId a, ParticipantId b) {
    return a < b ? DmKey{a, b} : DmKey{b, a}; // supporte réellement les IDs 64 bits.
}
Result<void> Messaging::reserve(ParticipantId actor, const std::string& body) {
    if (body.empty() || body.size() > kMaxChatBodyBytes || !valid_text(body))
        return Result<void>::failure({ErrorCode::InvalidArgument, "texte invalide", "messaging"});
    if (items_ >= 1024 || body.size() > 1024 * 1024 - bytes_)
        return Result<void>::failure({ErrorCode::CapacityExceeded, "quota de session atteint", "messaging"});
    auto& window = rates_[actor];
    const auto now = session_.now_ms();
    if (now < window.start_ms || now - window.start_ms >= 1000) window = {now, 0};
    if (window.count >= 10) return Result<void>::failure({ErrorCode::RateLimited, "quota temporel atteint", "messaging"});
    ++window.count; ++items_; bytes_ += body.size();
    return Result<void>::success();
}
bool Messaging::visible(const ChatMessage& message, ParticipantId actor) const {
    if (message.recipient) return session_.can(actor, Capability::SendDm) &&
        (message.sender == actor || *message.recipient == actor);
    return session_.can(actor, Capability::SendChat);
}
ChatMessage* Messaging::find_message(MessageId id) {
    for (auto& message : general_) if (message.id == id) return &message;
    for (auto& [key, messages] : dms_) {
        (void)key;
        for (auto& message : messages) if (message.id == id) return &message;
    }
    return nullptr;
}
Result<MessageId> Messaging::post_general(ParticipantId actor, const std::string& body) {
    if (!session_.can(actor, Capability::SendChat)) return Result<MessageId>::failure(denied());
    if (auto result = reserve(actor, body); !result) return Result<MessageId>::failure(result.error());
    ChatMessage message;
    message.id = MessageId{next_message_++}; message.sender = actor; message.body = body;
    message.sent_ms = session_.now_ms();
    const auto id = message.id;
    general_.push_back(std::move(message));
    return Result<MessageId>::success(id);
}
Result<MessageId> Messaging::post_dm(ParticipantId actor, ParticipantId peer, const std::string& body) {
    if (actor == peer || !session_.can(actor, Capability::SendDm) || !session_.can(peer, Capability::SendDm))
        return Result<MessageId>::failure(denied());
    if (auto result = reserve(actor, body); !result) return Result<MessageId>::failure(result.error());
    ChatMessage message;
    message.id = MessageId{next_message_++}; message.sender = actor; message.recipient = peer;
    message.body = body; message.sent_ms = session_.now_ms();
    const auto id = message.id;
    dms_[dm_key(actor, peer)].push_back(std::move(message));
    return Result<MessageId>::success(id);
}
Result<MessageId> Messaging::reply(MessageId parent_id, ParticipantId actor, const std::string& body) {
    const auto* parent = find_message(parent_id);
    if (!parent || !visible(*parent, actor)) return Result<MessageId>::failure(denied());
    // Copier les IDs AVANT post : une allocation du vector pourrait invalider parent.
    const auto peer = parent->recipient ? std::optional{parent->sender == actor ? *parent->recipient : parent->sender} : std::nullopt;
    auto result = peer ? post_dm(actor, *peer, body) : post_general(actor, body);
    if (result) find_message(result.value())->reply_to = parent_id; // DM reste privé.
    return result;
}
Result<void> Messaging::mark_read(ParticipantId actor, MessageId id) {
    auto* message = find_message(id);
    if (!message || !visible(*message, actor)) return Result<void>::failure(denied());
    message->readers.insert(actor);
    return Result<void>::success();
}
Result<std::vector<ChatMessage>> Messaging::general_channel(ParticipantId actor) const {
    if (!session_.can(actor, Capability::SendChat)) return Result<std::vector<ChatMessage>>::failure(denied());
    return Result<std::vector<ChatMessage>>::success(general_);
}
Result<std::vector<ChatMessage>> Messaging::dm_channel(ParticipantId actor, ParticipantId peer) const {
    if (!session_.can(actor, Capability::SendDm) || !session_.find_participant(peer))
        return Result<std::vector<ChatMessage>>::failure(denied());
    const auto it = dms_.find(dm_key(actor, peer));
    return Result<std::vector<ChatMessage>>::success(it == dms_.end() ? std::vector<ChatMessage>{} : it->second);
}
Result<ReportId> Messaging::report_issue(ParticipantId actor, IssueReport report) {
    if (!session_.can(actor, Capability::SubmitFeedback)) return Result<ReportId>::failure(denied());
    if (report.has_screenshot) return Result<ReportId>::failure({ErrorCode::Unsupported, "capture non implémentée", "messaging"});
    if (report.severity > Severity::Blocker) return Result<ReportId>::failure({ErrorCode::InvalidArgument, "sévérité inconnue", "messaging"});
    if (auto result = reserve(actor, report.description); !result) return Result<ReportId>::failure(result.error());
    report.id = ReportId{next_report_++}; report.reporter = actor;
    report.browser_context = session_.find_participant(actor)->browser_context;
    report.page_route.clear(); // une query supprimée ne suffit pas : les chemins peuvent aussi contenir un secret.
    report.created_ms = session_.now_ms();
    const auto id = report.id;
    reports_.push_back(std::move(report));
    return Result<ReportId>::success(id);
}
void Messaging::clear_all() {
    general_.clear(); dms_.clear(); reports_.clear(); rates_.clear(); items_ = 0; bytes_ = 0;
    // Les compteurs d'identité ne repartent PAS à zéro : pas de réponse à un ancien ID réutilisé.
}
} // namespace plpt::model
