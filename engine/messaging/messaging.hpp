#pragma once
// Modèle local seulement, sans transport. Collections bornées et commandes
// autorisées contre la session ; le destinataire d'un DM n'est jamais un bit-pack.
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../session/session.hpp"
namespace plpt::model {
inline constexpr std::size_t kMaxChatBodyBytes = 4096;
struct ChatMessage {
    MessageId id{};
    ParticipantId sender{};
    std::optional<ParticipantId> recipient;
    std::string body;
    std::uint64_t sent_ms = 0;
    std::optional<MessageId> reply_to;
    std::set<ParticipantId> readers;
};
enum class Severity : std::uint8_t { Informative, Minor, Major, Blocker };
struct IssueReport {
    ReportId id{};
    ParticipantId reporter{};
    ContextId browser_context{};
    std::string page_route; // toujours vidée à l'ingestion : pas de route brute potentiellement secrète.
    std::string description;
    Severity severity = Severity::Minor;
    std::uint64_t created_ms = 0;
    bool has_screenshot = false; // non supporté, refus explicite si demandé.
};
class Messaging {
public:
    explicit Messaging(Session& session) : session_(session) {}
    Result<MessageId> post_general(ParticipantId, const std::string&);
    Result<MessageId> post_dm(ParticipantId, ParticipantId, const std::string&);
    Result<MessageId> reply(MessageId, ParticipantId, const std::string&);
    Result<void> mark_read(ParticipantId, MessageId);
    Result<std::vector<ChatMessage>> general_channel(ParticipantId) const;
    Result<std::vector<ChatMessage>> dm_channel(ParticipantId actor, ParticipantId peer) const;
    Result<ReportId> report_issue(ParticipantId, IssueReport);
    // Accesseur hôte de test, jamais exposé au réseau ; pas de confidentialité vis-à-vis de l'hôte.
    const std::vector<IssueReport>& host_reports() const { return reports_; }
    void clear_all(); // libération logique, PAS une promesse d'effacement RAM/SSD.
private:
    using DmKey = std::pair<ParticipantId, ParticipantId>;
    struct Window { std::uint64_t start_ms = 0; unsigned count = 0; };
    Session& session_;
    std::vector<ChatMessage> general_;
    std::map<DmKey, std::vector<ChatMessage>> dms_;
    std::vector<IssueReport> reports_;
    std::map<ParticipantId, Window> rates_;
    std::uint64_t next_message_ = 1, next_report_ = 1;
    std::size_t items_ = 0, bytes_ = 0;
    static DmKey dm_key(ParticipantId, ParticipantId);
    ChatMessage* find_message(MessageId);
    Result<void> reserve(ParticipantId, const std::string&);
    bool visible(const ChatMessage&, ParticipantId) const;
};
} // namespace plpt::model
