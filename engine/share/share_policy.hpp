#pragma once
// DevLink — engine/share/share_policy.hpp — the SINGLE server-side authority on
// who may drive the host browser (ADR-018 §6). Pure logic, no I/O, no threads:
// ShareHost calls it under its own mutex, unit tests call it directly.
//
//   Mode A  Independent  : one browser context per guest; a guest drives only
//                          its own context.
//   Mode B  Collaborative: one shared context; exactly one controller at a time
//                          (exclusive lease with an epoch); others observe and
//                          may request control; transfers are FIFO.
//   Mode C  ObserveOnly  : one shared context; no guest input is ever accepted.
//
// The guest UI may hide buttons, but nothing here trusts the guest: every input
// record is authorised again, with replay rejection (strictly increasing
// per-participant sequence) and refusal after revocation or while paused.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

#include "../core/result.hpp"
#include "../core/types.hpp"

namespace plpt::share {

enum class ShareMode : std::uint8_t { Independent = 1, Collaborative = 2, ObserveOnly = 3 };

inline const char* share_mode_text(ShareMode mode) {
    switch (mode) {
    case ShareMode::Independent: return "test indépendant";
    case ShareMode::Collaborative: return "collaboration (contrôle exclusif)";
    case ShareMode::ObserveOnly: return "observation seule";
    }
    return "inconnu";
}
inline std::optional<ShareMode> share_mode_from_byte(std::uint8_t v) {
    if (v >= 1 && v <= 3) return static_cast<ShareMode>(v);
    return std::nullopt;
}

enum class InputDecision : std::uint8_t {
    Accepted,
    UnknownParticipant,
    Revoked,
    ObserveOnlyMode,
    NotController,
    Paused,
    Replayed,
    NoContext,
};

inline const char* input_decision_text(InputDecision d) {
    switch (d) {
    case InputDecision::Accepted: return "acceptée";
    case InputDecision::UnknownParticipant: return "participant inconnu";
    case InputDecision::Revoked: return "participant révoqué";
    case InputDecision::ObserveOnlyMode: return "mode observation seule";
    case InputDecision::NotController: return "contrôle détenu par un autre participant";
    case InputDecision::Paused: return "partage suspendu par l'hôte";
    case InputDecision::Replayed: return "séquence rejouée ou non croissante";
    case InputDecision::NoContext: return "contexte navigateur non prêt";
    }
    return "inconnu";
}

struct ParticipantShare {
    ParticipantId id{};
    std::string name;
    ContextId context{};           // Mode A: own context; B/C: the shared one
    std::uint64_t last_input_seq = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    bool revoked = false;
};

struct LeaseView {
    ParticipantId holder{};        // 0 = nobody controls the shared view
    std::uint64_t epoch = 0;       // incremented on every change of holder
    std::deque<ParticipantId> waiting;
};

class SharePolicy {
public:
    explicit SharePolicy(ShareMode mode) : mode_(mode) {}

    ShareMode mode() const { return mode_; }
    std::uint64_t revision() const { return revision_; }
    const LeaseView& lease() const { return lease_; }
    bool paused() const { return paused_; }
    void set_paused(bool paused) {
        if (paused_ != paused) { paused_ = paused; ++revision_; }
    }

    // Shared context for modes B and C (the host's primary context).
    void set_shared_context(ContextId ctx) { shared_ = ctx; ++revision_; }
    ContextId shared_context() const { return shared_; }

    Result<void> add(ParticipantId id, std::string name) {
        if (id.value == 0) return Result<void>::failure({ErrorCode::InvalidArgument, "participant nul", "share"});
        if (revoked_.count(id)) return Result<void>::failure({ErrorCode::PermissionDenied, "participant révoqué", "share"});
        auto& p = participants_[id];
        // Each admission issues a NEW media key, hence a new TLS channel: TLS
        // already makes replaying records across channels impossible, so the
        // sequence space restarts here. Within a channel, replays are refused.
        p.id = id;
        p.name = std::move(name);
        p.revoked = false;
        p.last_input_seq = 0;
        if (mode_ != ShareMode::Independent) p.context = shared_;
        ++revision_;
        return Result<void>::success();
    }
    void set_context(ParticipantId id, ContextId ctx) {
        auto it = participants_.find(id);
        if (it == participants_.end()) return;
        it->second.context = ctx;
        ++revision_;
    }
    // Disconnection (may come back with its resume secret) or revocation (never).
    void remove(ParticipantId id, bool revoked) {
        auto it = participants_.find(id);
        if (it == participants_.end()) return;
        if (revoked) it->second.revoked = true;
        drop_from_lease(id);
        // A merely disconnected participant keeps its record AND (mode A) its
        // own browser context for the resume grace period: zeroing it here
        // would orphan a live context and create a second one on resume.
        if (revoked) {
            participants_.erase(it);
            revoked_.insert_or_assign(id, true);
        }
        ++revision_;
    }
    const ParticipantShare* find(ParticipantId id) const {
        auto it = participants_.find(id);
        return it == participants_.end() ? nullptr : &it->second;
    }
    const std::map<ParticipantId, ParticipantShare>& participants() const { return participants_; }

    // ---- Mode B lease ----
    // Returns true if the caller now holds the lease; false if queued/refused.
    bool request_control(ParticipantId id) {
        if (mode_ != ShareMode::Collaborative || !present(id)) return false;
        if (lease_.holder == id) return true;
        if (lease_.holder.value == 0) {
            set_holder(id);
            return true;
        }
        if (std::find(lease_.waiting.begin(), lease_.waiting.end(), id) == lease_.waiting.end()) {
            lease_.waiting.push_back(id);
            ++revision_;
        }
        return false;
    }
    void release_control(ParticipantId id) {
        if (mode_ != ShareMode::Collaborative) return;
        if (lease_.holder == id) pass_lease();
        else drop_waiting(id);
    }
    // Host decision: give control to `id` (or to nobody with id = 0).
    bool host_grant(ParticipantId id) {
        if (mode_ != ShareMode::Collaborative) return false;
        if (id.value != 0 && !present(id)) return false;
        drop_waiting(id);
        set_holder(id);
        return true;
    }

    // ---- The check performed for EVERY input record ----
    InputDecision authorize(ParticipantId id, std::uint64_t input_seq, ContextId* target) {
        if (revoked_.count(id)) return InputDecision::Revoked;
        auto it = participants_.find(id);
        if (it == participants_.end()) return InputDecision::UnknownParticipant;
        auto& p = it->second;
        auto reject = [&](InputDecision d) { ++p.rejected; return d; };
        if (p.revoked) return reject(InputDecision::Revoked);
        if (input_seq <= p.last_input_seq) return reject(InputDecision::Replayed);
        // The sequence is consumed even when refused below: a refused record can
        // never be replayed later to slip through after a lease change.
        p.last_input_seq = input_seq;
        if (mode_ == ShareMode::ObserveOnly) return reject(InputDecision::ObserveOnlyMode);
        if (paused_) return reject(InputDecision::Paused);
        if (mode_ == ShareMode::Collaborative && lease_.holder != id) return reject(InputDecision::NotController);
        if (p.context.value == 0) return reject(InputDecision::NoContext);
        if (target) *target = p.context;
        ++p.accepted;
        return InputDecision::Accepted;
    }
    bool may_input(ParticipantId id) const {
        auto it = participants_.find(id);
        if (it == participants_.end() || it->second.revoked || revoked_.count(id) || paused_) return false;
        if (mode_ == ShareMode::ObserveOnly) return false;
        if (mode_ == ShareMode::Collaborative) return lease_.holder == id;
        return it->second.context.value != 0;
    }

private:
    bool present(ParticipantId id) const {
        auto it = participants_.find(id);
        return it != participants_.end() && !it->second.revoked;
    }
    void set_holder(ParticipantId id) {
        lease_.holder = id;
        ++lease_.epoch;
        ++revision_;
    }
    void pass_lease() {
        ParticipantId next{};
        while (!lease_.waiting.empty() && next.value == 0) {
            const auto candidate = lease_.waiting.front();
            lease_.waiting.pop_front();
            if (present(candidate)) next = candidate;
        }
        set_holder(next);
    }
    void drop_waiting(ParticipantId id) {
        const auto before = lease_.waiting.size();
        lease_.waiting.erase(std::remove(lease_.waiting.begin(), lease_.waiting.end(), id), lease_.waiting.end());
        if (before != lease_.waiting.size()) ++revision_;
    }
    void drop_from_lease(ParticipantId id) {
        drop_waiting(id);
        if (lease_.holder == id) pass_lease();
    }

    ShareMode mode_;
    ContextId shared_{};
    bool paused_ = false;
    std::uint64_t revision_ = 1;
    LeaseView lease_;
    std::map<ParticipantId, ParticipantShare> participants_;
    std::map<ParticipantId, bool> revoked_;
};

} // namespace plpt::share
