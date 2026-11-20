#pragma once
// DevLink — engine/browser/webview2_backend.hpp
// Real Windows Browser Worker based on Microsoft Edge WebView2 (ADR-018).
//
//  - One dedicated STA thread owns every WebView2 object and pumps its messages;
//    the public API is thread-safe and never blocks the UI thread for long.
//  - One context = one off-screen host window + one InPrivate WebView2 profile:
//    cookies, cache, localStorage and sessionStorage are independent per context
//    (Mode A) and discarded when the context is destroyed.
//  - Real pixels: the Chromium compositor stream (DevTools `Page.startScreencast`
//    through the official CallDevToolsProtocolMethod API), JPEG encoded by the
//    browser, ack-based backpressure, newest-frame-wins slot per context.
//  - Real input: DevTools `Input.dispatchMouseEvent` / `dispatchKeyEvent` /
//    `insertText` (trusted events, no window focus needed).
//  - Allowlist enforced on navigations, redirects, sub-frames, every
//    sub-resource request, popups, downloads, permissions and external schemes.
#include "browser.hpp"
#include "egress_guard.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace plpt::browser {

// Was the WebView2 SDK present at compile time? This says the backend CODE is
// in the binary. Whether a browser really starts on this machine is a runtime
// fact, established by creating one (see probe()), never assumed.
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

// The real backend is implemented wherever the SDK is compiled in.
inline constexpr bool kWebView2BackendImplemented = kWebView2SdkCompiled;
// Kept for source compatibility: the backend is operational code, but sharing
// availability is still decided at runtime by is_available()/probe().
inline constexpr bool kWebView2BackendOperational = kWebView2BackendImplemented;

struct WebView2Options {
    std::uint32_t viewport_width = 1280;
    std::uint32_t viewport_height = 720;
    int jpeg_quality = 70;                       // 1..100, browser-side JPEG
    std::chrono::milliseconds operation_timeout{20000};
    std::string base_temp_dir;                   // empty = %TEMP%\devlink_browser
};

class WebView2Backend final : public IRemoteBrowserBackend {
public:
    explicit WebView2Backend(std::string base_temp_dir = "");
    explicit WebView2Backend(WebView2Options options);
    ~WebView2Backend() override;

    WebView2Backend(const WebView2Backend&) = delete;
    WebView2Backend& operator=(const WebView2Backend&) = delete;

    // runtime_available: the loader DLL (next to the executable) and an installed
    // Evergreen runtime are both present. is_available: additionally, the backend
    // is compiled in. Neither proves a browser starts: probe() does.
    static bool runtime_available();
    static std::string runtime_version();
    static bool is_available();

    Result<BrowserContextInfo> create_context(ParticipantId owner, const BrowserPolicy& policy) override;
    Result<void> navigate(ContextId ctx, const std::string& url) override;
    Result<void> destroy_context(ContextId ctx) override;
    // Returns a copy of the latest real JPEG frame of the context.
    Result<std::vector<std::uint8_t>> capture_frame(ContextId ctx) override;
    // `event` is a serialized media::InputEvent (validated again here).
    Result<void> inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) override;
    Result<std::shared_ptr<const CapturedFrame>> wait_frame(ContextId ctx, std::uint64_t after_sequence,
        std::chrono::milliseconds timeout) override;
    Result<std::string> evaluate_for_diagnostics(ContextId ctx, const std::string& script) override;
    Result<BrowserContextStats> context_stats(ContextId ctx) override;
    bool real_engine() const override { return kWebView2BackendImplemented; }

    std::size_t active_contexts() const;
    // Socket-level egress of the browser process (SOCKS5 guard, ADR-018 §4).
    EgressCounters egress_counters() const;
    // Destroys every context, releases the environment, waits (bounded) for the
    // browser process to exit and removes the temporary user-data folder.
    // Idempotent. Called by the destructor.
    void cleanup_all();
    // Browser process of the environment (0 if none was started).
    std::uint32_t browser_process_id() const;
    // After cleanup_all(): did the browser process exit and the folder vanish?
    bool last_cleanup_complete() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Synthetic backend reserved for explicitly labelled demonstrations/tests.
// Never use this factory to claim that a real browser exists.
std::unique_ptr<IRemoteBrowserBackend> create_demo_browser_backend(const std::string& custom_temp_dir = "");

} // namespace plpt::browser
