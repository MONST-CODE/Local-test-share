#pragma once
// DevLink — engine/browser/webview2_backend.hpp
// Implémentation native Windows de IRemoteBrowserBackend basée sur Microsoft Edge WebView2.
// Mandat Trajectoire 2 (§14 Browser Worker, §14.1 Profils indépendants, §14.2 Allowlist stricte).
#include "browser.hpp"
#include <memory>
#include <string>

namespace plpt::browser {

// Le SDK WebView2 était-il présent à la compilation ? Cela ne signifie PAS que
// le backend est opérationnel : l'environnement/contrôleur, la capture et l'entrée
// restent à implémenter et à qualifier end-to-end. En attendant, la disponibilité
// de partage réel reste explicitement fausse sur toutes les plateformes.
#if defined(__has_include)
#if defined(_WIN32) && __has_include(<WebView2.h>)
inline constexpr bool kWebView2SdkCompiled = true;
#else
inline constexpr bool kWebView2SdkCompiled = false;
#endif
#else
#if defined(_WIN32)
inline constexpr bool kWebView2SdkCompiled = true;
#else
inline constexpr bool kWebView2SdkCompiled = false;
#endif
#endif

inline constexpr bool kWebView2BackendOperational = false; // à basculer uniquement après qualification E2E réelle.

class WebView2Backend final : public IRemoteBrowserBackend {
public:
    explicit WebView2Backend(std::string base_temp_dir = "");
    ~WebView2Backend() override;

    WebView2Backend(const WebView2Backend&) = delete;
    WebView2Backend& operator=(const WebView2Backend&) = delete;

    // is_available décrit le partage réellement utilisable ; runtime_available
    // ne mesure que la présence du chargeur WebView2 installé.
    static bool is_available();
    static bool runtime_available();

    Result<BrowserContextInfo> create_context(ParticipantId owner, const BrowserPolicy& policy) override;
    Result<void> navigate(ContextId ctx, const std::string& origin) override;
    Result<void> destroy_context(ContextId ctx) override;
    Result<std::vector<std::uint8_t>> capture_frame(ContextId ctx) override;
    Result<void> inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) override;

    std::size_t active_contexts() const;
    void cleanup_all();

};

// Backend synthétique réservé aux démonstrations/tests explicitement signalés.
// Ne jamais utiliser cette fabrique pour prétendre qu'un navigateur réel existe.
std::unique_ptr<IRemoteBrowserBackend> create_demo_browser_backend(const std::string& custom_temp_dir = "");

} // namespace plpt::browser
