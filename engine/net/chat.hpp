#pragma once
// PLPT — engine/net/chat.hpp — ligne de messagerie partagée par l'hôte et l'invité.
// L'hôte reste l'autorité (modèle Messaging : quotas, DM, visibilité) ; l'invité
// ne conserve qu'une boîte de réception bornée de ce qu'il a réellement reçu.
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include "../core/types.hpp"

namespace plpt::net {

struct ChatLine {
    std::uint64_t id = 0;
    ParticipantId sender{};
    std::string sender_name;
    std::string body;
    std::uint64_t sent_ms = 0;
    bool system = false;   // true = événement de session, jamais un utilisateur.
    bool direct = false;   // true = message privé, visible uniquement par ses deux extrémités.
    ParticipantId recipient{}; // destinataire d'un message privé (0 = canal général).
};

// Signalement d'anomalie (protocole 1.5) : invité -> hôte uniquement. La
// gravité suit model::Severity ; `context` est le texte SAISI par l'invité
// (écran, route, étape), jamais une URL capturée automatiquement.
enum class FeedbackSeverity : std::uint8_t { Informative = 0, Minor = 1, Major = 2, Blocker = 3 };

inline const char* feedback_severity_text(FeedbackSeverity severity) {
    switch (severity) {
    case FeedbackSeverity::Informative: return "information";
    case FeedbackSeverity::Minor: return "mineure";
    case FeedbackSeverity::Major: return "majeure";
    case FeedbackSeverity::Blocker: return "bloquante";
    }
    return "inconnue";
}

struct FeedbackEntry {
    std::uint64_t id = 0;              // identifiant attribué par l'hôte
    ParticipantId reporter{};
    std::string reporter_name;
    FeedbackSeverity severity = FeedbackSeverity::Minor;
    std::string description;
    std::string context;
    std::uint64_t received_ms = 0;     // horloge murale de l'hôte
};

// Boîte de réception bornée : un hôte malveillant ou bavard ne peut pas faire
// croître la mémoire de l'invité sans limite.
class BoundedChat {
public:
    explicit BoundedChat(std::size_t capacity = 200) : capacity_(capacity == 0 ? 1 : capacity) {}
    void push(ChatLine line) {
        lines_.push_back(std::move(line));
        while (lines_.size() > capacity_) lines_.pop_front();
    }
    std::vector<ChatLine> lines() const { return {lines_.begin(), lines_.end()}; }
    std::size_t size() const { return lines_.size(); }
    void clear() { lines_.clear(); }
private:
    std::deque<ChatLine> lines_;
    std::size_t capacity_;
};

} // namespace plpt::net
