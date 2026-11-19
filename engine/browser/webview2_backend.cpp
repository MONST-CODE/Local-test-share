// DevLink — engine/browser/webview2_backend.cpp
// WebView2 n'est PAS encore un backend de partage opérationnel : il manque la
// création du contrôleur/CoreWebView2, l'acquisition de pixels et l'injection.
// Ce fichier échoue donc fermé, même si le runtime Windows est installé.
#include "webview2_backend.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace plpt::browser {

bool WebView2Backend::runtime_available() {
#if defined(_WIN32)
    static HMODULE loader = LoadLibraryExW(L"WebView2Loader.dll", nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    return loader && GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions") != nullptr;
#else
    return false;
#endif
}

// Runtime présent ne signifie pas que le backend sait partager une page. Cette
// capacité ne deviendra vraie qu'après implémentation et tests E2E du pipeline.
bool WebView2Backend::is_available() {
    return runtime_available() && kWebView2BackendOperational;
}

WebView2Backend::WebView2Backend(std::string) {}
WebView2Backend::~WebView2Backend() = default;

Result<BrowserContextInfo> WebView2Backend::create_context(ParticipantId, const BrowserPolicy&) {
    return Result<BrowserContextInfo>::failure(Error{ErrorCode::PlatformError,
        "WebView2 non opérationnel : création du contrôleur navigateur non implémentée", "browser"});
}
Result<void> WebView2Backend::navigate(ContextId, const std::string&) {
    return Result<void>::failure(Error{ErrorCode::PlatformError,
        "WebView2 non opérationnel : navigation réelle non implémentée", "browser"});
}
Result<void> WebView2Backend::destroy_context(ContextId) {
    return Result<void>::success();
}
Result<std::vector<std::uint8_t>> WebView2Backend::capture_frame(ContextId) {
    return Result<std::vector<std::uint8_t>>::failure(Error{ErrorCode::PlatformError,
        "WebView2 non opérationnel : capture réelle non implémentée", "browser"});
}
Result<void> WebView2Backend::inject_input(ContextId, const std::vector<std::uint8_t>&) {
    return Result<void>::failure(Error{ErrorCode::PlatformError,
        "WebView2 non opérationnel : injection d'entrée réelle non implémentée", "browser"});
}
std::size_t WebView2Backend::active_contexts() const { return 0; }
void WebView2Backend::cleanup_all() {}

// Cette fabrique est réservée au backend synthétique de démonstration/tests.
// Le poste de commande ne l'utilise jamais pour déclarer un moteur réel.
std::unique_ptr<IRemoteBrowserBackend> create_demo_browser_backend(const std::string& custom_temp_dir) {
    return std::make_unique<DemoBrowserBackend>(custom_temp_dir);
}

} // namespace plpt::browser
