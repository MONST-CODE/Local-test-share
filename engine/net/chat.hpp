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
