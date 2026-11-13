#pragma once
// PLPT — engine/browser/browser.hpp — §8.8, §12.4.
// Interface IRemoteBrowserBackend indépendante du fournisseur de moteur Web.
// Prototype Windows recommandé : WebView2 (composition, profils distincts, R12/R13).
// La faisabilité capture hors écran basse latence est À PROUVER (Phase 4, §25).
// Aucun moteur HTML/CSS/JS n'est réécrit (§2.4) ; l'adaptateur est remplaçable.
#include <string>
#include <vector>

#include "../core/types.hpp"
#include "../core/result.hpp"

namespace plpt {

struct BrowserPolicy {
    std::vector<std::string> allowed_origins; // allowlist stricte (§12.5)
    bool allow_downloads = false;
    bool allow_clipboard = false;
    bool allow_file_drag_drop = false;
    bool allow_popups = false;
    bool allow_external_protocols = false;
};

struct BrowserContextInfo {
    ContextId context{};
    ParticipantId owner{};
    std::string profile_id;
    std::string current_origin;
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

} // namespace plpt
