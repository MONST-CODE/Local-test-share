#pragma once
// DevLink (formerly PLPT) — engine/browser/browser.hpp — §8.8, §12.4, §25-§28.
// Interface IRemoteBrowserBackend et implémentations d'isolation de navigation.
// - Profils indépendants par invité (cookies, cache et stockage local distincts).
// - Allowlist stricte (§28) interdisant file://, protocoles externes et hôtes non autorisés.
// - Capture de frames et injection d'événements.
// - Nettoyage complet des profils temporaires à la destruction (§37).
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/types.hpp"
#include "../core/result.hpp"
#include "url_origin.hpp"

namespace plpt {

struct BrowserPolicy {
    std::vector<std::string> allowed_origins; // allowlist stricte (§12.5, §28)
    bool allow_downloads = false;
    bool allow_clipboard = false;
    bool allow_file_drag_drop = false;
    bool allow_popups = false;
    bool allow_external_protocols = false;

    bool is_origin_allowed(std::string_view target) const {
        // Compare canonical HTTP(S) origins, not string prefixes. In particular,
        // `http://allowed.example.attacker/` must not match `http://allowed.example`.
        // Wildcards, userinfo, non-HTTP schemes, malformed authorities and
        // non-canonical IP literals fail closed.
        return browser::url_origin::is_allowed(target, allowed_origins);
    }
};

struct BrowserContextInfo {
    ContextId context{};
    ParticipantId owner{};
    std::string profile_id;
    std::string current_origin;
};

// A frame really produced by the browser compositor of one context. `data` is
// the complete JPEG bitstream (media::FrameCodec::Jpeg); width/height are read
// from the JPEG itself, never assumed. Immutable once published.
struct CapturedFrame {
    std::uint64_t sequence = 0;      // strictly increasing per context, starts at 1
    std::uint64_t captured_ns = 0;   // steady clock, when the compositor delivered it
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> data;
};

// Honest per-context counters for the UI, diagnostics and qualification.
struct BrowserContextStats {
    bool navigated = false;              // last navigation completed successfully
    int last_http_status = 0;
    std::uint64_t frames = 0;            // compositor frames received
    std::uint64_t frame_bytes = 0;
    std::uint64_t blocked_navigations = 0;
    std::uint64_t blocked_requests = 0;  // sub-resources refused by the allowlist
    std::uint64_t blocked_popups = 0;
    std::uint64_t blocked_downloads = 0;
    std::uint64_t denied_permissions = 0;
    std::uint64_t inputs = 0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::string current_url;             // host-side only, never sent to guests
};

class IRemoteBrowserBackend {
public:
    virtual ~IRemoteBrowserBackend() = default;
    virtual Result<BrowserContextInfo> create_context(ParticipantId owner, const BrowserPolicy& policy) = 0;
    virtual Result<void> navigate(ContextId ctx, const std::string& origin) = 0;
    virtual Result<void> destroy_context(ContextId ctx) = 0;
    // Capture : retourne une frame brute (l'encodage est dans capture_encode/).
    virtual Result<std::vector<std::uint8_t>> capture_frame(ContextId ctx) = 0;
    virtual Result<void> inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) = 0;

    // Real frame pipeline (ADR-018). Waits up to `timeout` for a frame whose
    // sequence is greater than `after_sequence` and returns the LATEST one:
    // intermediate frames are skipped on purpose, latency never accumulates.
    // Backends without a real compositor fail closed.
    virtual Result<std::shared_ptr<const CapturedFrame>> wait_frame(ContextId, std::uint64_t /*after_sequence*/,
        std::chrono::milliseconds /*timeout*/) {
        return Result<std::shared_ptr<const CapturedFrame>>::failure(
            Error{ErrorCode::Unsupported, "ce backend ne produit pas de frames réelles", "browser"});
    }
    // HOST-SIDE diagnostics/qualification only (reads DOM state to prove that an
    // injected event really changed the page). Never reachable from a guest.
    virtual Result<std::string> evaluate_for_diagnostics(ContextId, const std::string& /*script*/) {
        return Result<std::string>::failure(
            Error{ErrorCode::Unsupported, "évaluation indisponible sur ce backend", "browser"});
    }
    virtual Result<BrowserContextStats> context_stats(ContextId) {
        return Result<BrowserContextStats>::failure(
            Error{ErrorCode::Unsupported, "statistiques indisponibles sur ce backend", "browser"});
    }
    // True only for a backend that runs a real browser engine.
    virtual bool real_engine() const { return false; }
};

// Backend nul : échoue explicitement. Utilisé pour prouver que le coeur ne
// dépend pas d'un navigateur réel et que les erreurs sont explicites.
class NullBrowserBackend final : public IRemoteBrowserBackend {
public:
    Result<BrowserContextInfo> create_context(ParticipantId, const BrowserPolicy&) override {
        return Result<BrowserContextInfo>::failure(
            Error{ErrorCode::PlatformError, "aucun backend navigateur (prototype Phase 4 requis)", "browser"});
    }
    Result<void> navigate(ContextId, const std::string&) override {
        return Result<void>::failure(Error{ErrorCode::PlatformError, "backend nul", "browser"});
    }
    Result<void> destroy_context(ContextId) override { return Result<void>::success(); }
    Result<std::vector<std::uint8_t>> capture_frame(ContextId) override {
        return Result<std::vector<std::uint8_t>>::failure(Error{ErrorCode::PlatformError, "backend nul", "browser"});
    }
    Result<void> inject_input(ContextId, const std::vector<std::uint8_t>&) override {
        return Result<void>::failure(Error{ErrorCode::PlatformError, "backend nul", "browser"});
    }
};

// Backend de démonstration uniquement : les pixels sont synthétiques et aucune
// entrée n'est transmise à un navigateur. Il ne doit jamais servir de preuve de
// partage ni être sélectionné implicitement comme backend de production.
class DemoBrowserBackend final : public IRemoteBrowserBackend {
public:
    explicit DemoBrowserBackend(std::string base_temp_dir = "")
        : base_temp_dir_(base_temp_dir.empty() ? default_temp_dir() : std::move(base_temp_dir)) {}

    ~DemoBrowserBackend() override {
        cleanup_all();
    }

    DemoBrowserBackend(const DemoBrowserBackend&) = delete;
    DemoBrowserBackend& operator=(const DemoBrowserBackend&) = delete;

    Result<BrowserContextInfo> create_context(ParticipantId owner, const BrowserPolicy& policy) override {
        if (owner.value == 0) {
            return Result<BrowserContextInfo>::failure(
                Error{ErrorCode::InvalidArgument, "propriétaire de contexte invalide", "browser"});
        }
        const ContextId id{next_context_id_++};
        const std::string profile_id = "devlink_prof_" + std::to_string(owner.value) + "_" + std::to_string(id.value);
        std::string temp_path = (std::filesystem::path(base_temp_dir_) / profile_id).string();

        std::error_code ec;
        std::filesystem::create_directories(temp_path, ec);

        ContextSlot slot;
        slot.info.context = id;
        slot.info.owner = owner;
        slot.info.profile_id = profile_id;
        slot.policy = policy;
        slot.profile_path = temp_path;

        contexts_[id] = std::move(slot);
        return Result<BrowserContextInfo>::success(contexts_[id].info);
    }

    Result<void> navigate(ContextId ctx, const std::string& origin) override {
        auto it = contexts_.find(ctx);
        if (it == contexts_.end()) {
            return Result<void>::failure(
                Error{ErrorCode::NotFound, "contexte introuvable", "browser"});
        }
        if (!it->second.policy.is_origin_allowed(origin)) {
            return Result<void>::failure(
                Error{ErrorCode::PermissionDenied, "navigation refusée par la politique d'isolation", "browser"});
        }
        it->second.info.current_origin = origin;
        it->second.has_navigated = true;
        it->second.frame_counter++;
        return Result<void>::success();
    }

    Result<void> destroy_context(ContextId ctx) override {
        auto it = contexts_.find(ctx);
        if (it == contexts_.end()) return Result<void>::success();

        // Nettoyage immédiat du profil temporaire (§37)
        if (!it->second.profile_path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(it->second.profile_path, ec);
        }
        contexts_.erase(it);
        return Result<void>::success();
    }

    Result<std::vector<std::uint8_t>> capture_frame(ContextId ctx) override {
        auto it = contexts_.find(ctx);
        if (it == contexts_.end()) {
            return Result<std::vector<std::uint8_t>>::failure(
                Error{ErrorCode::NotFound, "contexte introuvable", "browser"});
        }
        // Synthèse d'une trame de composition 32-bit RGBA (128x64 minimale pour transport bas coût)
        // Format: [Largeur: u16][Hauteur: u16][Compteur: u32][Pixels RGBA...]
        constexpr std::uint16_t width = 128;
        constexpr std::uint16_t height = 64;
        const std::uint32_t count = it->second.frame_counter++;
        std::vector<std::uint8_t> frame(8 + width * height * 4);
        frame[0] = static_cast<std::uint8_t>(width >> 8);
        frame[1] = static_cast<std::uint8_t>(width & 0xFF);
        frame[2] = static_cast<std::uint8_t>(height >> 8);
        frame[3] = static_cast<std::uint8_t>(height & 0xFF);
        frame[4] = static_cast<std::uint8_t>((count >> 24) & 0xFF);
        frame[5] = static_cast<std::uint8_t>((count >> 16) & 0xFF);
        frame[6] = static_cast<std::uint8_t>((count >> 8) & 0xFF);
        frame[7] = static_cast<std::uint8_t>(count & 0xFF);

        // Gradient de rendu reflétant l'état du navigateur
        const std::uint8_t r_base = it->second.has_navigated ? 20 : 60;
        const std::uint8_t g_base = it->second.has_navigated ? 160 : 60;
        const std::uint8_t b_base = 120;
        std::size_t offset = 8;
        for (std::uint16_t y = 0; y < height; ++y) {
            for (std::uint16_t x = 0; x < width; ++x) {
                frame[offset++] = static_cast<std::uint8_t>((r_base + (x % 32)) & 0xFF);
                frame[offset++] = static_cast<std::uint8_t>((g_base + (y % 32)) & 0xFF);
                frame[offset++] = b_base;
                frame[offset++] = 255;
            }
        }
        return Result<std::vector<std::uint8_t>>::success(std::move(frame));
    }

    Result<void> inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) override {
        auto it = contexts_.find(ctx);
        if (it == contexts_.end()) {
            return Result<void>::failure(
                Error{ErrorCode::NotFound, "contexte introuvable", "browser"});
        }
        if (event.empty()) {
            return Result<void>::failure(
                Error{ErrorCode::InvalidArgument, "événement d'entrée vide", "browser"});
        }
        it->second.injected_inputs_count++;
        return Result<void>::success();
    }

    std::size_t active_contexts() const { return contexts_.size(); }

    void cleanup_all() {
        for (auto& [id, slot] : contexts_) {
            if (!slot.profile_path.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(slot.profile_path, ec);
            }
        }
        contexts_.clear();
    }

private:
    struct ContextSlot {
        BrowserContextInfo info;
        BrowserPolicy policy;
        std::string profile_path;
        bool has_navigated = false;
        std::uint32_t frame_counter = 0;
        std::uint64_t injected_inputs_count = 0;
    };

    static std::string default_temp_dir() {
        std::error_code ec;
        auto temp = std::filesystem::temp_directory_path(ec);
        if (ec) return ".";
        return (temp / "devlink_browser").string();
    }

    std::string base_temp_dir_;
    std::uint64_t next_context_id_ = 1;
    std::map<ContextId, ContextSlot> contexts_;
};

} // namespace plpt
