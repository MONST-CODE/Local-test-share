#pragma once
// DevLink — engine/browser/webview2_backend.hpp
// Implémentation native Windows de IRemoteBrowserBackend basée sur Microsoft Edge WebView2.
// Mandat Trajectoire 2 (§14 Browser Worker, §14.1 Profils indépendants, §14.2 Allowlist stricte).
#include "browser.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace plpt::browser {

class WebView2Backend final : public IRemoteBrowserBackend {
public:
    explicit WebView2Backend(std::string base_temp_dir = "");
    ~WebView2Backend() override;

    WebView2Backend(const WebView2Backend&) = delete;
    WebView2Backend& operator=(const WebView2Backend&) = delete;

    static bool is_available();

    Result<BrowserContextInfo> create_context(ParticipantId owner, const BrowserPolicy& policy) override;
    Result<void> navigate(ContextId ctx, const std::string& origin) override;
    Result<void> destroy_context(ContextId ctx) override;
    Result<std::vector<std::uint8_t>> capture_frame(ContextId ctx) override;
    Result<void> inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) override;

    std::size_t active_contexts() const;
    void cleanup_all();

private:
    struct ContextSlot {
        BrowserContextInfo info;
        BrowserPolicy policy;
        std::filesystem::path profile_path;
        void* hwnd_offscreen = nullptr;
        void* webview_env = nullptr;
        void* webview_controller = nullptr;
        void* webview_core = nullptr;
        bool has_navigated = false;
        std::uint32_t frame_counter = 0;
        std::uint64_t injected_inputs = 0;
        std::vector<std::uint8_t> last_frame;
    };

    mutable std::mutex mutex_;
    std::filesystem::path base_temp_dir_;
    std::uint64_t next_context_id_ = 1;
    std::map<ContextId, ContextSlot> contexts_;
    void* loader_module_ = nullptr;
};

// Fabrique sélectionnant WebView2 sous Windows si disponible, avec repli automatique sur ManagedBrowserBackend
std::unique_ptr<IRemoteBrowserBackend> create_browser_backend(const std::string& custom_temp_dir = "");

} // namespace plpt::browser
