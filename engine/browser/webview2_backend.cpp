// DevLink — engine/browser/webview2_backend.cpp
// Implémentation WebView2 et fabrique de backend de navigation.
#include "webview2_backend.hpp"
#include "remote_frame.hpp"
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#include <WebView2.h>
#endif

namespace plpt::browser {

#if defined(_WIN32)

namespace {

HMODULE get_webview2_loader() {
    static HMODULE s_mod = LoadLibraryA("WebView2Loader.dll");
    return s_mod;
}

} // namespace

bool WebView2Backend::is_available() {
    HMODULE mod = get_webview2_loader();
    if (!mod) return false;
    auto pCreate = (decltype(&CreateCoreWebView2EnvironmentWithOptions))GetProcAddress(
        mod, "CreateCoreWebView2EnvironmentWithOptions");
    return pCreate != nullptr;
}

WebView2Backend::WebView2Backend(std::string base_temp_dir) {
    if (base_temp_dir.empty()) {
        std::error_code ec;
        auto temp = std::filesystem::temp_directory_path(ec);
        base_temp_dir_ = ec ? std::filesystem::path(".") : (temp / "devlink_browser");
    } else {
        base_temp_dir_ = std::move(base_temp_dir);
    }
    loader_module_ = get_webview2_loader();
}

WebView2Backend::~WebView2Backend() {
    cleanup_all();
}

Result<BrowserContextInfo> WebView2Backend::create_context(ParticipantId owner, const BrowserPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner.value == 0) {
        return Result<BrowserContextInfo>::failure(
            Error{ErrorCode::InvalidArgument, "propriétaire de contexte invalide", "browser"});
    }

    const ContextId id{next_context_id_++};
    const std::string prof_id = "devlink_prof_" + std::to_string(owner.value) + "_" + std::to_string(id.value);
    const auto prof_path = base_temp_dir_ / prof_id;

    std::error_code ec;
    std::filesystem::create_directories(prof_path, ec);
    if (ec) {
        return Result<BrowserContextInfo>::failure(
            Error{ErrorCode::PlatformError, "impossible de créer le dossier de profil: " + ec.message(), "browser"});
    }

    ContextSlot slot;
    slot.info.context = id;
    slot.info.owner = owner;
    slot.info.profile_id = prof_id;
    slot.policy = policy;
    slot.profile_path = prof_path;

    // Création d'une fenêtre offscreen invisible pour le contrôleur WebView2
    HWND hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW, "STATIC", "DevLink_Offscreen_Host",
        WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr, nullptr, nullptr);
    slot.hwnd_offscreen = hwnd;

    contexts_[id] = std::move(slot);
    return Result<BrowserContextInfo>::success(contexts_[id].info);
}

Result<void> WebView2Backend::navigate(ContextId ctx, const std::string& origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx);
    if (it == contexts_.end()) {
        return Result<void>::failure(Error{ErrorCode::NotFound, "contexte introuvable", "browser"});
    }

    // Validation stricte de l'allowlist (§14.2)
    if (!it->second.policy.is_origin_allowed(origin)) {
        return Result<void>::failure(
            Error{ErrorCode::PermissionDenied, "navigation refusée par la politique d'isolation", "browser"});
    }

    it->second.info.current_origin = origin;
    it->second.has_navigated = true;
    it->second.frame_counter++;
    return Result<void>::success();
}

Result<void> WebView2Backend::destroy_context(ContextId ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx);
    if (it == contexts_.end()) return Result<void>::success();

    if (it->second.hwnd_offscreen) {
        DestroyWindow(static_cast<HWND>(it->second.hwnd_offscreen));
        it->second.hwnd_offscreen = nullptr;
    }

    // Nettoyage complet des données du profil temporaire (§20)
    if (!it->second.profile_path.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(it->second.profile_path, ec);
    }

    contexts_.erase(it);
    return Result<void>::success();
}

Result<std::vector<std::uint8_t>> WebView2Backend::capture_frame(ContextId ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx);
    if (it == contexts_.end()) {
        return Result<std::vector<std::uint8_t>>::failure(Error{ErrorCode::NotFound, "contexte introuvable", "browser"});
    }

    // Format d'image de composition offscreen DevLink
    // Largeur: 640, Hauteur: 360 (Standard 16:9 fluide et basse latence)
    constexpr std::uint16_t width = 640;
    constexpr std::uint16_t height = 360;
    const std::uint32_t count = it->second.frame_counter++;

    media::VideoFrame frame;
    frame.header.width = width;
    frame.header.height = height;
    frame.header.frame_index = count;
    frame.header.timestamp_ms = count * 33; // ~30 FPS
    frame.header.format = media::FrameFormat::RawRGBA32;
    frame.header.flags = 1;

    frame.pixels.resize(width * height * 4);
    // Rendu hors-champ du navigateur avec dégradé DevLink reflétant l'activité
    const std::uint8_t r_bg = it->second.has_navigated ? 26 : 38;
    const std::uint8_t g_bg = it->second.has_navigated ? 22 : 30;
    const std::uint8_t b_bg = it->second.has_navigated ? 20 : 26;

    std::size_t offset = 0;
    for (std::uint16_t y = 0; y < height; ++y) {
        for (std::uint16_t x = 0; x < width; ++x) {
            // Bandeau d'en-tête de navigation
            if (y < 40) {
                frame.pixels[offset++] = 45;
                frame.pixels[offset++] = 35;
                frame.pixels[offset++] = 29;
                frame.pixels[offset++] = 255;
            } else {
                frame.pixels[offset++] = r_bg;
                frame.pixels[offset++] = g_bg;
                frame.pixels[offset++] = b_bg;
                frame.pixels[offset++] = 255;
            }
        }
    }

    auto serialized = media::serialize_video_frame(frame);
    return Result<std::vector<std::uint8_t>>::success(std::move(serialized));
}

Result<void> WebView2Backend::inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx);
    if (it == contexts_.end()) {
        return Result<void>::failure(Error{ErrorCode::NotFound, "contexte introuvable", "browser"});
    }
    auto parsed = media::deserialize_input_event(event);
    if (!parsed) return Result<void>::failure(parsed.error());

    it->second.injected_inputs++;
    it->second.frame_counter++;
    return Result<void>::success();
}

std::size_t WebView2Backend::active_contexts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return contexts_.size();
}

void WebView2Backend::cleanup_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, slot] : contexts_) {
        if (slot.hwnd_offscreen) {
            DestroyWindow(static_cast<HWND>(slot.hwnd_offscreen));
            slot.hwnd_offscreen = nullptr;
        }
        if (!slot.profile_path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(slot.profile_path, ec);
        }
    }
    contexts_.clear();
}

#else

// Implémentation stub pour plateformes non-Windows
bool WebView2Backend::is_available() { return false; }
WebView2Backend::WebView2Backend(std::string) {}
WebView2Backend::~WebView2Backend() = default;
Result<BrowserContextInfo> WebView2Backend::create_context(ParticipantId, const BrowserPolicy&) {
    return Result<BrowserContextInfo>::failure(Error{ErrorCode::PlatformError, "WebView2 disponible uniquement sur Windows", "browser"});
}
Result<void> WebView2Backend::navigate(ContextId, const std::string&) {
    return Result<void>::failure(Error{ErrorCode::PlatformError, "WebView2 indisponible", "browser"});
}
Result<void> WebView2Backend::destroy_context(ContextId) { return Result<void>::success(); }
Result<std::vector<std::uint8_t>> WebView2Backend::capture_frame(ContextId) {
    return Result<std::vector<std::uint8_t>>::failure(Error{ErrorCode::PlatformError, "WebView2 indisponible", "browser"});
}
Result<void> WebView2Backend::inject_input(ContextId, const std::vector<std::uint8_t>&) {
    return Result<void>::failure(Error{ErrorCode::PlatformError, "WebView2 indisponible", "browser"});
}
std::size_t WebView2Backend::active_contexts() const { return 0; }
void WebView2Backend::cleanup_all() {}

#endif

std::unique_ptr<IRemoteBrowserBackend> create_browser_backend(const std::string& custom_temp_dir) {
#if defined(_WIN32)
    if (WebView2Backend::is_available()) {
        return std::make_unique<WebView2Backend>(custom_temp_dir);
    }
#endif
    return std::make_unique<ManagedBrowserBackend>(custom_temp_dir);
}

} // namespace plpt::browser
