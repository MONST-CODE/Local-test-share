// DevLink — engine/browser/webview2_backend.cpp — real WebView2 Browser Worker.
// See webview2_backend.hpp and docs/ADR/ADR-018-browser-worker-webview2-cdp.md.
#include "webview2_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "cdp_json.hpp"
#include "egress_guard.hpp"
#include "remote_frame.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_WIN32) && __has_include(<WebView2.h>)
#define DEVLINK_HAVE_WEBVIEW2 1
#include <objbase.h>
#include <WebView2.h>
#include <wrl/client.h>
#endif

namespace plpt::browser {

namespace {
Error browser_error(ErrorCode code, std::string message, std::int64_t native = 0) {
    return Error{code, std::move(message), "browser", native};
}
} // namespace

#if !defined(DEVLINK_HAVE_WEBVIEW2)
// ---------------------------------------------------------------------------
// Platforms without WebView2: the backend exists but fails closed, explicitly.
// ---------------------------------------------------------------------------
struct WebView2Backend::Impl {};
bool WebView2Backend::runtime_available() { return false; }
std::string WebView2Backend::runtime_version() { return {}; }
bool WebView2Backend::is_available() { return false; }
WebView2Backend::WebView2Backend(std::string) {}
WebView2Backend::WebView2Backend(WebView2Options) {}
WebView2Backend::~WebView2Backend() = default;
namespace {
Error unavailable() {
    return browser_error(ErrorCode::PlatformError, "WebView2 indisponible : backend propre à Windows, SDK absent de cette compilation");
}
} // namespace
Result<BrowserContextInfo> WebView2Backend::create_context(ParticipantId, const BrowserPolicy&) { return Result<BrowserContextInfo>::failure(unavailable()); }
Result<void> WebView2Backend::navigate(ContextId, const std::string&) { return Result<void>::failure(unavailable()); }
Result<void> WebView2Backend::destroy_context(ContextId) { return Result<void>::success(); }
Result<std::vector<std::uint8_t>> WebView2Backend::capture_frame(ContextId) { return Result<std::vector<std::uint8_t>>::failure(unavailable()); }
Result<void> WebView2Backend::inject_input(ContextId, const std::vector<std::uint8_t>&) { return Result<void>::failure(unavailable()); }
Result<std::shared_ptr<const CapturedFrame>> WebView2Backend::wait_frame(ContextId, std::uint64_t, std::chrono::milliseconds) {
    return Result<std::shared_ptr<const CapturedFrame>>::failure(unavailable());
}
Result<std::string> WebView2Backend::evaluate_for_diagnostics(ContextId, const std::string&) { return Result<std::string>::failure(unavailable()); }
Result<BrowserContextStats> WebView2Backend::context_stats(ContextId) { return Result<BrowserContextStats>::failure(unavailable()); }
std::size_t WebView2Backend::active_contexts() const { return 0; }
EgressCounters WebView2Backend::egress_counters() const { return {}; }
void WebView2Backend::cleanup_all() {}
std::uint32_t WebView2Backend::browser_process_id() const { return 0; }
bool WebView2Backend::last_cleanup_complete() const { return true; }
#else
// ---------------------------------------------------------------------------
// Windows implementation
// ---------------------------------------------------------------------------
using Microsoft::WRL::ComPtr;

namespace {

// ---- Loader (WebView2Loader.dll next to the executable, never from CWD/PATH) ----
using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using GetVersionFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);

struct Loader {
    HMODULE module = nullptr;
    CreateEnvironmentFn create = nullptr;
    GetVersionFn version = nullptr;
};
const Loader& loader() {
    static const Loader instance = [] {
        Loader l;
        l.module = LoadLibraryExW(L"WebView2Loader.dll", nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (l.module) {
            l.create = reinterpret_cast<CreateEnvironmentFn>(
                reinterpret_cast<void*>(GetProcAddress(l.module, "CreateCoreWebView2EnvironmentWithOptions")));
            l.version = reinterpret_cast<GetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(l.module, "GetAvailableCoreWebView2BrowserVersionString")));
        }
        return l;
    }();
    return instance;
}

std::wstring to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::string to_utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}
// Takes ownership of a CoTaskMem string returned by WebView2.
std::string take_string(LPWSTR text) {
    std::string out = to_utf8(text);
    if (text) CoTaskMemFree(text);
    return out;
}

std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// MinGW has no __uuidof for the WebView2 interfaces: always pass the IID.
template <class T, class U> HRESULT query(const ComPtr<U>& from, const IID& iid, ComPtr<T>& to) {
    if (!from) return E_POINTER;
    return from->QueryInterface(iid, reinterpret_cast<void**>(to.ReleaseAndGetAddressOf()));
}

// ---- Minimal COM callback object (no WRL runtime dependency) ----
template <class I, class... A>
class Callback final : public I {
public:
    Callback(const IID& iid, std::function<HRESULT(A...)> fn) : iid_(iid), fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... args) override { return fn_ ? fn_(args...) : S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    IID iid_;
    std::function<HRESULT(A...)> fn_;
};
template <class I, class M> struct CallbackFor;
template <class I, class R, class C, class... A> struct CallbackFor<I, R (STDMETHODCALLTYPE C::*)(A...)> {
    using type = Callback<I, A...>;
};
template <class I, class F> ComPtr<I> make_callback(const IID& iid, F&& fn) {
    using T = typename CallbackFor<I, decltype(&I::Invoke)>::type;
    ComPtr<I> ptr;
    ptr.Attach(new T(iid, std::forward<F>(fn)));
    return ptr;
}

// ---- One-shot completion shared between the caller thread and the STA ----
template <class T> struct Completion {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<Result<T>> value;
    void set(Result<T> v) {
        std::lock_guard lock(mutex);
        if (!value) value.emplace(std::move(v));
        cv.notify_all();
    }
    std::optional<Result<T>> wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, timeout, [&] { return value.has_value(); });
        if (!value) return std::nullopt;
        return std::move(*value);
    }
};

// ---- DevTools key table (Windows virtual-key codes -> DOM key/code) ----
struct KeyNames { const char* key; const char* code; };
std::optional<KeyNames> key_names(std::uint32_t vk) {
    switch (vk) {
    case 0x08: return KeyNames{"Backspace", "Backspace"};
    case 0x09: return KeyNames{"Tab", "Tab"};
    case 0x0D: return KeyNames{"Enter", "Enter"};
    case 0x10: return KeyNames{"Shift", "ShiftLeft"};
    case 0x11: return KeyNames{"Control", "ControlLeft"};
    case 0x12: return KeyNames{"Alt", "AltLeft"};
    case 0x14: return KeyNames{"CapsLock", "CapsLock"};
    case 0x1B: return KeyNames{"Escape", "Escape"};
    case 0x20: return KeyNames{" ", "Space"};
    case 0x21: return KeyNames{"PageUp", "PageUp"};
    case 0x22: return KeyNames{"PageDown", "PageDown"};
    case 0x23: return KeyNames{"End", "End"};
    case 0x24: return KeyNames{"Home", "Home"};
    case 0x25: return KeyNames{"ArrowLeft", "ArrowLeft"};
    case 0x26: return KeyNames{"ArrowUp", "ArrowUp"};
    case 0x27: return KeyNames{"ArrowRight", "ArrowRight"};
    case 0x28: return KeyNames{"ArrowDown", "ArrowDown"};
    case 0x2D: return KeyNames{"Insert", "Insert"};
    case 0x2E: return KeyNames{"Delete", "Delete"};
    case 0x5B: return KeyNames{"Meta", "MetaLeft"};
    default: break;
    }
    static const char* const kLetters[] = {"a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z"};
    static const char* const kLetterCodes[] = {"KeyA","KeyB","KeyC","KeyD","KeyE","KeyF","KeyG","KeyH","KeyI","KeyJ","KeyK","KeyL","KeyM","KeyN","KeyO","KeyP","KeyQ","KeyR","KeyS","KeyT","KeyU","KeyV","KeyW","KeyX","KeyY","KeyZ"};
    static const char* const kDigits[] = {"0","1","2","3","4","5","6","7","8","9"};
    static const char* const kDigitCodes[] = {"Digit0","Digit1","Digit2","Digit3","Digit4","Digit5","Digit6","Digit7","Digit8","Digit9"};
    static const char* const kF[] = {"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"};
    if (vk >= 'A' && vk <= 'Z') return KeyNames{kLetters[vk - 'A'], kLetterCodes[vk - 'A']};
    if (vk >= '0' && vk <= '9') return KeyNames{kDigits[vk - '0'], kDigitCodes[vk - '0']};
    if (vk >= 0x70 && vk <= 0x7B) return KeyNames{kF[vk - 0x70], kF[vk - 0x70]};
    return std::nullopt;
}

// media modifier bits (Shift=1, Ctrl=2, Alt=4, Meta=8) -> CDP (Alt=1, Ctrl=2, Meta=4, Shift=8)
int cdp_modifiers(std::uint16_t m) {
    int out = 0;
    if (m & 0x1) out |= 8;
    if (m & 0x2) out |= 2;
    if (m & 0x4) out |= 1;
    if (m & 0x8) out |= 4;
    return out;
}
const char* cdp_button(std::uint8_t button) {
    switch (button) {
    case 1: return "left";
    case 2: return "middle";
    case 3: return "right";
    case 4: return "back";
    case 5: return "forward";
    default: return "none";
    }
}
int cdp_button_mask(std::uint8_t button) {
    switch (button) {
    case 1: return 1;
    case 2: return 4;
    case 3: return 2;
    case 4: return 8;
    case 5: return 16;
    default: return 0;
    }
}

constexpr UINT kTaskMessage = WM_APP + 0x31;
const wchar_t kWorkerWindowClass[] = L"DevLinkBrowserWorkerHost";
const wchar_t kTaskWindowClass[] = L"DevLinkBrowserWorkerTasks";

std::string random_suffix() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%lu-%llx", static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(counter.QuadPart));
    return buffer;
}

} // namespace

// Frames are produced on the STA thread and consumed by any thread.
struct FrameSlot {
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<const CapturedFrame> latest;
    BrowserContextStats stats;
    bool closed = false;
};

struct WebView2Backend::Impl {
    struct Context {
        ContextId id{};
        ParticipantId owner{};
        BrowserPolicy policy;
        std::string profile;
        HWND window = nullptr;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> frame_receiver;
        EventRegistrationToken frame_token{};
        std::shared_ptr<FrameSlot> slot;
        bool egress_registered = false;
        std::uint64_t next_sequence = 1;
        // navigation tracking
        std::shared_ptr<Completion<void>> navigation;
        bool navigation_blocked = false;
        // input state
        int pressed_mask = 0;
        std::uint8_t last_press_button = 0;
        double last_press_x = -100, last_press_y = -100;
        std::uint64_t last_press_ms = 0;
        int click_count = 0;
    };

    explicit Impl(WebView2Options o) : options(std::move(o)) {
        if (options.base_temp_dir.empty()) {
            std::error_code ec;
            auto temp = std::filesystem::temp_directory_path(ec);
            options.base_temp_dir = ec ? std::string(".") : (temp / "devlink_browser").string();
        }
        options.jpeg_quality = std::clamp(options.jpeg_quality, 10, 95);
        options.viewport_width = std::clamp<std::uint32_t>(options.viewport_width, 320, media::kMaxFrameWidth);
        options.viewport_height = std::clamp<std::uint32_t>(options.viewport_height, 240, media::kMaxFrameHeight);
    }
    ~Impl() { shutdown(); }

    // ---------------- STA thread ----------------
    bool ensure_thread() {
        std::lock_guard lock(thread_mutex);
        if (thread_running) return true;
        if (thread_failed) return false;
        auto ready = std::make_shared<Completion<void>>();
        thread = std::thread([this, ready] { thread_main(ready); });
        auto started = ready->wait(std::chrono::seconds(10));
        if (!started || !*started) {
            thread_failed = true;
            if (thread.joinable()) {
                if (task_window) PostMessageW(task_window, WM_QUIT, 0, 0);
                thread.join();
            }
            return false;
        }
        thread_running = true;
        return true;
    }

    static LRESULT CALLBACK task_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == kTaskMessage) {
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self) self->run_tasks();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void thread_main(std::shared_ptr<Completion<void>> ready) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(co)) {
            ready->set(Result<void>::failure(browser_error(ErrorCode::PlatformError, "CoInitializeEx STA impossible", co)));
            return;
        }
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW host{};
        host.cbSize = sizeof host;
        host.lpfnWndProc = DefWindowProcW;
        host.hInstance = instance;
        host.lpszClassName = kWorkerWindowClass;
        RegisterClassExW(&host);
        WNDCLASSEXW task_class{};
        task_class.cbSize = sizeof task_class;
        task_class.lpfnWndProc = &Impl::task_proc;
        task_class.hInstance = instance;
        task_class.lpszClassName = kTaskWindowClass;
        RegisterClassExW(&task_class);
        task_window = CreateWindowExW(0, kTaskWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (!task_window) {
            ready->set(Result<void>::failure(browser_error(ErrorCode::PlatformError, "fenêtre de tâches impossible",
                static_cast<std::int64_t>(GetLastError()))));
            CoUninitialize();
            return;
        }
        SetWindowLongPtrW(task_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ready->set(Result<void>::success());
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        run_tasks();
        // Final release on the owning thread.
        for (auto& [id, ctx] : contexts) close_context(*ctx);
        contexts.clear();
        environment.Reset();
        DestroyWindow(task_window);
        task_window = nullptr;
        CoUninitialize();
    }

    void run_tasks() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard lock(task_mutex);
                if (tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

    bool post(std::function<void()> task) {
        if (!ensure_thread()) return false;
        {
            std::lock_guard lock(task_mutex);
            tasks.push_back(std::move(task));
        }
        return PostMessageW(task_window, kTaskMessage, 0, 0) != 0;
    }

    template <class T>
    Result<T> call(std::function<void(std::shared_ptr<Completion<T>>)> start, std::chrono::milliseconds timeout,
        const char* what) {
        auto completion = std::make_shared<Completion<T>>();
        if (!post([start = std::move(start), completion]() mutable { start(completion); }))
            return Result<T>::failure(browser_error(ErrorCode::PlatformError,
                "thread navigateur indisponible (STA WebView2)"));
        auto result = completion->wait(timeout);
        if (!result) return Result<T>::failure(browser_error(ErrorCode::TimedOut, std::string("délai dépassé : ") + what));
        return std::move(*result);
    }

    // ---------------- Environment ----------------
    void with_environment(std::function<void(Result<void>)> next) {
        if (environment) { next(Result<void>::success()); return; }
        const Loader& l = loader();
        if (!l.create) {
            next(Result<void>::failure(browser_error(ErrorCode::PlatformError,
                "WebView2Loader.dll absent du dossier de l'exécutable")));
            return;
        }
        std::error_code ec;
        user_data_folder = (std::filesystem::path(options.base_temp_dir) / ("udf-" + random_suffix())).string();
        std::filesystem::create_directories(user_data_folder, ec);
        if (ec) {
            next(Result<void>::failure(browser_error(ErrorCode::IoError, "dossier de profil temporaire impossible")));
            return;
        }
        // Socket-level boundary FIRST: no browser process exists without it.
        auto guard_port = guard.start();
        if (!guard_port) {
            next(Result<void>::failure(browser_error(ErrorCode::PlatformError,
                "garde réseau du navigateur impossible : " + guard_port.error().message)));
            return;
        }
        // Documented WebView2 mechanism for Chromium switches; set before the
        // environment is created (process-wide, read once at creation).
        //  - every connection goes through the DevLink egress guard, loopback
        //    included (<-loopback> removes Chromium's implicit bypass);
        //  - off-screen worker windows keep rendering at full rate.
        const std::wstring args =
            L"--proxy-server=socks5://127.0.0.1:" + std::to_wstring(guard_port.value()) +
            L" --proxy-bypass-list=<-loopback>"
            L" --disable-features=CalculateNativeWinOcclusion,msSmartScreenProtection"
            L" --disable-background-timer-throttling --disable-backgrounding-occluded-windows"
            L" --disable-renderer-backgrounding --no-first-run";
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args.c_str());
        auto handler = make_callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [this, next](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    next(Result<void>::failure(browser_error(ErrorCode::PlatformError,
                        "création de l'environnement WebView2 refusée", hr)));
                    return S_OK;
                }
                environment = env;
                ComPtr<ICoreWebView2Environment5> env5;
                if (SUCCEEDED(query(environment, IID_ICoreWebView2Environment5, env5))) {
                    auto exited = make_callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                        IID_ICoreWebView2BrowserProcessExitedEventHandler,
                        [this](ICoreWebView2Environment*, ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
                            browser_exited.store(true);
                            return S_OK;
                        });
                    env5->add_BrowserProcessExited(exited.Get(), &exit_token);
                }
                next(Result<void>::success());
                return S_OK;
            });
        const std::wstring folder = to_wide(user_data_folder);
        const HRESULT hr = l.create(nullptr, folder.c_str(), nullptr, handler.Get());
        if (FAILED(hr))
            next(Result<void>::failure(browser_error(ErrorCode::PlatformError,
                "runtime WebView2 introuvable ou refusé (Evergreen requis)", hr)));
    }

    // ---------------- Context creation ----------------
    void start_create(std::shared_ptr<Completion<BrowserContextInfo>> done, ParticipantId owner, BrowserPolicy policy) {
        with_environment([this, done, owner, policy](Result<void> env) {
            if (!env) { done->set(Result<BrowserContextInfo>::failure(env.error())); return; }
            auto ctx = std::make_unique<Context>();
            ctx->id = ContextId{next_context++};
            ctx->owner = owner;
            ctx->policy = policy;
            ctx->profile = "devlink_p" + std::to_string(owner.value) + "_c" + std::to_string(ctx->id.value);
            ctx->slot = std::make_shared<FrameSlot>();
            const int w = static_cast<int>(options.viewport_width);
            const int h = static_cast<int>(options.viewport_height);
            // Off-screen, never activated, never in the taskbar: a guest never
            // sees the host desktop, and the host never sees a stray window.
            const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - w - 200;
            const int y = GetSystemMetrics(SM_YVIRTUALSCREEN) - h - 200;
            ctx->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWorkerWindowClass, L"DevLink Browser Worker",
                WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!ctx->window) {
                done->set(Result<BrowserContextInfo>::failure(browser_error(ErrorCode::PlatformError,
                    "fenêtre hôte du navigateur impossible", static_cast<std::int64_t>(GetLastError()))));
                return;
            }
            ShowWindow(ctx->window, SW_SHOWNOACTIVATE);
            ComPtr<ICoreWebView2Environment10> env10;
            ComPtr<ICoreWebView2ControllerOptions> opts;
            if (FAILED(query(environment, IID_ICoreWebView2Environment10, env10)) || FAILED(env10->CreateCoreWebView2ControllerOptions(&opts))) {
                DestroyWindow(ctx->window);
                done->set(Result<BrowserContextInfo>::failure(browser_error(ErrorCode::Unsupported,
                    "runtime WebView2 trop ancien : profils indépendants indisponibles")));
                return;
            }
            const std::wstring profile = to_wide(ctx->profile);
            opts->put_ProfileName(profile.c_str());
            opts->put_IsInPrivateModeEnabled(TRUE);
            Context* raw = ctx.get();
            pending_contexts[raw->id] = std::move(ctx);
            auto handler = make_callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                [this, done, raw](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                    auto owned = std::move(pending_contexts[raw->id]);
                    pending_contexts.erase(raw->id);
                    if (FAILED(hr) || !controller) {
                        DestroyWindow(raw->window);
                        done->set(Result<BrowserContextInfo>::failure(browser_error(ErrorCode::PlatformError,
                            "création du contrôleur WebView2 refusée", hr)));
                        return S_OK;
                    }
                    owned->controller = controller;
                    auto configured = configure(*owned);
                    if (!configured) {
                        close_context(*owned);
                        done->set(Result<BrowserContextInfo>::failure(configured.error()));
                        return S_OK;
                    }
                    BrowserContextInfo info;
                    info.context = owned->id;
                    info.owner = owned->owner;
                    info.profile_id = owned->profile;
                    {
                        std::lock_guard lock(slots_mutex);
                        slots[owned->id] = owned->slot;
                    }
                    if (auto added = guard.add_origins(owned->policy.allowed_origins); !added) {
                        close_context(*owned);
                        done->set(Result<BrowserContextInfo>::failure(added.error()));
                        return S_OK;
                    }
                    owned->egress_registered = true;
                    if (browser_pid == 0) owned->webview->get_BrowserProcessId(&browser_pid);
                    contexts[owned->id] = std::move(owned);
                    active.store(contexts.size());
                    done->set(Result<BrowserContextInfo>::success(info));
                    return S_OK;
                });
            const HRESULT hr = env10->CreateCoreWebView2ControllerWithOptions(raw->window, opts.Get(), handler.Get());
            if (FAILED(hr)) {
                auto owned = std::move(pending_contexts[raw->id]);
                pending_contexts.erase(raw->id);
                DestroyWindow(owned->window);
                done->set(Result<BrowserContextInfo>::failure(browser_error(ErrorCode::PlatformError,
                    "création du contrôleur WebView2 impossible", hr)));
            }
        });
    }

    void count(Context& ctx, std::uint64_t BrowserContextStats::*field) {
        std::lock_guard lock(ctx.slot->mutex);
        ++(ctx.slot->stats.*field);
    }

    Result<void> configure(Context& ctx) {
        HRESULT hr = ctx.controller->get_CoreWebView2(&ctx.webview);
        if (FAILED(hr) || !ctx.webview)
            return Result<void>::failure(browser_error(ErrorCode::PlatformError, "CoreWebView2 indisponible", hr));
        RECT bounds{0, 0, static_cast<LONG>(options.viewport_width), static_cast<LONG>(options.viewport_height)};
        ctx.controller->put_Bounds(bounds);
        ctx.controller->put_IsVisible(TRUE);
        ComPtr<ICoreWebView2Controller3> c3;
        if (SUCCEEDED(query(ctx.controller, IID_ICoreWebView2Controller3, c3))) {
            c3->put_ShouldDetectMonitorScaleChanges(FALSE);
            c3->put_RasterizationScale(1.0);
        }
        // ---- Settings: nothing that reaches the host beyond the page ----
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(ctx.webview->get_Settings(&settings)) && settings) {
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreHostObjectsAllowed(FALSE);
            settings->put_IsWebMessageEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            ComPtr<ICoreWebView2Settings3> s3;
            if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings3, s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            ComPtr<ICoreWebView2Settings4> s4;
            if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings4, s4))) {
                s4->put_IsPasswordAutosaveEnabled(FALSE);
                s4->put_IsGeneralAutofillEnabled(FALSE);
            }
            ComPtr<ICoreWebView2Settings6> s6;
            if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings6, s6))) s6->put_IsSwipeNavigationEnabled(FALSE);
        }
        Context* raw = &ctx;
        EventRegistrationToken token{};
        // ---- Navigation allowlist: top-level, redirects and sub-frames ----
        auto on_navigation = [this, raw](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            const std::string url = take_string(uri);
            if (url == "about:blank" || raw->policy.is_origin_allowed(url)) return S_OK;
            args->put_Cancel(TRUE);
            raw->navigation_blocked = true;
            count(*raw, &BrowserContextStats::blocked_navigations);
            return S_OK;
        };
        ctx.webview->add_NavigationStarting(make_callback<ICoreWebView2NavigationStartingEventHandler>(
            IID_ICoreWebView2NavigationStartingEventHandler, on_navigation).Get(), &token);
        ctx.webview->add_FrameNavigationStarting(make_callback<ICoreWebView2NavigationStartingEventHandler>(
            IID_ICoreWebView2NavigationStartingEventHandler, on_navigation).Get(), &token);
        ctx.webview->add_NavigationCompleted(make_callback<ICoreWebView2NavigationCompletedEventHandler>(
            IID_ICoreWebView2NavigationCompletedEventHandler,
            [raw](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok = FALSE;
                args->get_IsSuccess(&ok);
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_WebErrorStatus(&status);
                int http = 0;
                ComPtr<ICoreWebView2NavigationCompletedEventArgs2> a2;
                if (SUCCEEDED(args->QueryInterface(IID_ICoreWebView2NavigationCompletedEventArgs2,
                        reinterpret_cast<void**>(a2.GetAddressOf()))))
                    a2->get_HttpStatusCode(&http);
                LPWSTR source = nullptr;
                raw->webview->get_Source(&source);
                const std::string current = take_string(source);
                {
                    std::lock_guard lock(raw->slot->mutex);
                    raw->slot->stats.navigated = ok == TRUE;
                    raw->slot->stats.last_http_status = http;
                    raw->slot->stats.current_url = current;
                }
                if (raw->navigation) {
                    auto pending = std::move(raw->navigation);
                    if (raw->navigation_blocked)
                        pending->set(Result<void>::failure(browser_error(ErrorCode::PermissionDenied,
                            "navigation bloquée par la politique d'origines (redirection ou cible hors liste)")));
                    else if (!ok)
                        pending->set(Result<void>::failure(browser_error(ErrorCode::IoError,
                            "chargement de la page échoué (statut WebView2 " + std::to_string(static_cast<int>(status)) + ")",
                            static_cast<int>(status))));
                    else
                        pending->set(Result<void>::success());
                }
                return S_OK;
            }).Get(), &token);
        // ---- Popups: never a new window, never a new origin ----
        ctx.webview->add_NewWindowRequested(make_callback<ICoreWebView2NewWindowRequestedEventHandler>(
            IID_ICoreWebView2NewWindowRequestedEventHandler,
            [this, raw](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE); // no window is created
                count(*raw, &BrowserContextStats::blocked_popups);
                return S_OK;
            }).Get(), &token);
        // ---- Every sub-resource (fetch, XHR, img, script, iframe...) ----
        hr = ctx.webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(hr))
            return Result<void>::failure(browser_error(ErrorCode::PlatformError, "filtre de requêtes impossible", hr));
        ctx.webview->add_WebResourceRequested(make_callback<ICoreWebView2WebResourceRequestedEventHandler>(
            IID_ICoreWebView2WebResourceRequestedEventHandler,
            [this, raw](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                if (FAILED(args->get_Request(&request)) || !request) return S_OK;
                LPWSTR uri = nullptr;
                request->get_Uri(&uri);
                const std::string url = take_string(uri);
                if (raw->policy.is_origin_allowed(url)) return S_OK;
                if (url.rfind("data:", 0) == 0 || url.rfind("blob:", 0) == 0) return S_OK; // in-page, no network
                ComPtr<ICoreWebView2WebResourceResponse> response;
                environment->CreateWebResourceResponse(nullptr, 403, L"Forbidden by DevLink origin policy",
                    L"Content-Type: text/plain", &response);
                args->put_Response(response.Get());
                count(*raw, &BrowserContextStats::blocked_requests);
                return S_OK;
            }).Get(), &token);
        // ---- Permissions (clipboard read, camera, geolocation, ...) ----
        ctx.webview->add_PermissionRequested(make_callback<ICoreWebView2PermissionRequestedEventHandler>(
            IID_ICoreWebView2PermissionRequestedEventHandler,
            [this, raw](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                args->get_PermissionKind(&kind);
                const bool clipboard = kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ;
                args->put_State(clipboard && raw->policy.allow_clipboard ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                                                                         : COREWEBVIEW2_PERMISSION_STATE_DENY);
                if (!(clipboard && raw->policy.allow_clipboard)) count(*raw, &BrowserContextStats::denied_permissions);
                return S_OK;
            }).Get(), &token);
        // ---- Downloads ----
        ComPtr<ICoreWebView2_4> wv4;
        if (SUCCEEDED(query(ctx.webview, IID_ICoreWebView2_4, wv4))) {
            wv4->add_DownloadStarting(make_callback<ICoreWebView2DownloadStartingEventHandler>(
                IID_ICoreWebView2DownloadStartingEventHandler,
                [this, raw](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                    if (raw->policy.allow_downloads) return S_OK;
                    args->put_Cancel(TRUE);
                    args->put_Handled(TRUE);
                    count(*raw, &BrowserContextStats::blocked_downloads);
                    return S_OK;
                }).Get(), &token);
        } else {
            return Result<void>::failure(browser_error(ErrorCode::Unsupported, "blocage des téléchargements indisponible"));
        }
        // ---- External protocols (mailto:, ms-settings:, custom schemes) ----
        ComPtr<ICoreWebView2_18> wv18;
        if (SUCCEEDED(query(ctx.webview, IID_ICoreWebView2_18, wv18))) {
            wv18->add_LaunchingExternalUriScheme(make_callback<ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
                IID_ICoreWebView2LaunchingExternalUriSchemeEventHandler,
                [this, raw](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* args) -> HRESULT {
                    if (raw->policy.allow_external_protocols) return S_OK;
                    args->put_Cancel(TRUE);
                    count(*raw, &BrowserContextStats::blocked_navigations);
                    return S_OK;
                }).Get(), &token);
        }
        // ---- Real compositor frames ----
        hr = ctx.webview->GetDevToolsProtocolEventReceiver(L"Page.screencastFrame", &ctx.frame_receiver);
        if (FAILED(hr) || !ctx.frame_receiver)
            return Result<void>::failure(browser_error(ErrorCode::PlatformError, "flux de rendu indisponible", hr));
        ctx.frame_receiver->add_DevToolsProtocolEventReceived(make_callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            IID_ICoreWebView2DevToolsProtocolEventReceivedEventHandler,
            [this, raw](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
                LPWSTR json = nullptr;
                if (FAILED(args->get_ParameterObjectAsJson(&json))) return S_OK;
                on_frame(*raw, take_string(json));
                return S_OK;
            }).Get(), &ctx.frame_token);
        {
            std::lock_guard lock(ctx.slot->mutex);
            ctx.slot->stats.viewport_width = options.viewport_width;
            ctx.slot->stats.viewport_height = options.viewport_height;
        }
        const std::string metrics = "{\"width\":" + std::to_string(options.viewport_width) +
            ",\"height\":" + std::to_string(options.viewport_height) +
            ",\"deviceScaleFactor\":1,\"mobile\":false}";
        cdp(ctx, "Emulation.setDeviceMetricsOverride", metrics);
        cdp(ctx, "Emulation.setFocusEmulationEnabled", "{\"enabled\":true}");
        cdp(ctx, "Page.enable", "{}");
        const std::string screencast = "{\"format\":\"jpeg\",\"quality\":" + std::to_string(options.jpeg_quality) +
            ",\"maxWidth\":" + std::to_string(options.viewport_width) + ",\"maxHeight\":" +
            std::to_string(options.viewport_height) + ",\"everyNthFrame\":1}";
        cdp(ctx, "Page.startScreencast", screencast);
        return Result<void>::success();
    }

    void cdp(Context& ctx, const char* method, const std::string& params,
        std::function<void(HRESULT, std::string)> done = {}) {
        if (!ctx.webview) {
            if (done) done(E_FAIL, {});
            return;
        }
        const std::wstring m = to_wide(method);
        const std::wstring p = to_wide(params);
        auto handler = make_callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            IID_ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
            [done](HRESULT hr, LPCWSTR result) -> HRESULT {
                if (done) done(hr, to_utf8(result));
                return S_OK;
            });
        const HRESULT hr = ctx.webview->CallDevToolsProtocolMethod(m.c_str(), p.c_str(), handler.Get());
        if (FAILED(hr) && done) done(hr, {});
    }

    void on_frame(Context& ctx, const std::string& json) {
        auto parsed = cdp::parse(json);
        if (!parsed) return;
        const auto session = parsed->number_at("sessionId");
        const auto data = parsed->string_at("data");
        if (session) {
            // Ack first: Chromium only produces the next frame after the ack, so the
            // browser itself never queues stale frames for us.
            char ack[64];
            std::snprintf(ack, sizeof ack, "{\"sessionId\":%lld", static_cast<long long>(*session));
            cdp(ctx, "Page.screencastFrameAck", std::string(ack) + "}");
        }
        if (!data) return;
        auto jpeg = cdp::base64_decode(*data, media::kMaxCompressedPayloadBytes);
        if (!jpeg) return;
        const auto dims = media::jpeg_dimensions(*jpeg);
        if (!dims || dims->width > media::kMaxFrameWidth || dims->height > media::kMaxFrameHeight) return;
        auto frame = std::make_shared<CapturedFrame>();
        frame->sequence = ctx.next_sequence++;
        frame->captured_ns = steady_ns();
        frame->width = dims->width;
        frame->height = dims->height;
        frame->data = std::move(*jpeg);
        {
            std::lock_guard lock(ctx.slot->mutex);
            ctx.slot->stats.frames++;
            ctx.slot->stats.frame_bytes += frame->data.size();
            ctx.slot->latest = std::move(frame);
        }
        ctx.slot->cv.notify_all();
    }

    void close_context(Context& ctx) {
        if (ctx.egress_registered) {
            guard.remove_origins(ctx.policy.allowed_origins);
            ctx.egress_registered = false;
        }
        if (ctx.slot) {
            {
                std::lock_guard lock(ctx.slot->mutex);
                ctx.slot->closed = true;
            }
            ctx.slot->cv.notify_all();
        }
        if (ctx.navigation) {
            ctx.navigation->set(Result<void>::failure(browser_error(ErrorCode::Cancelled, "contexte détruit")));
            ctx.navigation.reset();
        }
        if (ctx.frame_receiver) {
            ctx.frame_receiver->remove_DevToolsProtocolEventReceived(ctx.frame_token);
            ctx.frame_receiver.Reset();
        }
        ctx.webview.Reset();
        if (ctx.controller) {
            ctx.controller->Close();
            ctx.controller.Reset();
        }
        if (ctx.window) {
            DestroyWindow(ctx.window);
            ctx.window = nullptr;
        }
    }

    Context* find(ContextId id) {
        auto it = contexts.find(id);
        return it == contexts.end() ? nullptr : it->second.get();
    }

    std::shared_ptr<FrameSlot> slot_of(ContextId id) {
        std::lock_guard lock(slots_mutex);
        auto it = slots.find(id);
        return it == slots.end() ? nullptr : it->second;
    }

    // ---------------- Input mapping (DevTools Input domain) ----------------
    void inject(Context& ctx, const media::InputEvent& ev) {
        const double w = static_cast<double>(options.viewport_width);
        const double h = static_cast<double>(options.viewport_height);
        const double x = std::clamp(static_cast<double>(ev.x_norm) * w, 0.0, w - 1.0);
        const double y = std::clamp(static_cast<double>(ev.y_norm) * h, 0.0, h - 1.0);
        const int mods = cdp_modifiers(ev.modifiers);
        char buffer[512];
        auto pointer = [&](const char* type, const char* button, int buttons, int clicks, double dy) {
            std::snprintf(buffer, sizeof buffer,
                "{\"type\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d,"
                "\"modifiers\":%d,\"deltaX\":0,\"deltaY\":%.2f,\"pointerType\":\"mouse\"}",
                type, x, y, button, buttons, clicks, mods, dy);
            cdp(ctx, "Input.dispatchMouseEvent", buffer);
        };
        switch (ev.type) {
        case media::InputEventType::MouseMove: {
            const char* held = (ctx.pressed_mask & 1) ? "left" : (ctx.pressed_mask & 2) ? "right" : (ctx.pressed_mask & 4) ? "middle" : "none";
            pointer("mouseMoved", held, ctx.pressed_mask, 0, 0.0);
            break;
        }
        case media::InputEventType::MouseDown: {
            const auto now = GetTickCount64();
            const bool repeat = ctx.last_press_button == ev.button && now - ctx.last_press_ms <= GetDoubleClickTime() &&
                std::abs(ctx.last_press_x - x) <= 4.0 && std::abs(ctx.last_press_y - y) <= 4.0;
            ctx.click_count = repeat ? std::min(ctx.click_count + 1, 3) : 1;
            ctx.last_press_button = ev.button;
            ctx.last_press_ms = now;
            ctx.last_press_x = x;
            ctx.last_press_y = y;
            ctx.pressed_mask |= cdp_button_mask(ev.button);
            pointer("mousePressed", cdp_button(ev.button), ctx.pressed_mask, ctx.click_count, 0.0);
            break;
        }
        case media::InputEventType::MouseUp:
            ctx.pressed_mask &= ~cdp_button_mask(ev.button);
            pointer("mouseReleased", cdp_button(ev.button), ctx.pressed_mask, std::max(ctx.click_count, 1), 0.0);
            break;
        case media::InputEventType::MouseWheel:
            // wheel_delta: 1/120 notch, positive = up. CDP deltaY: pixels, positive = down.
            pointer("mouseWheel", "none", ctx.pressed_mask, 0, -static_cast<double>(ev.wheel_delta) * 100.0 / 120.0);
            break;
        case media::InputEventType::KeyDown:
        case media::InputEventType::KeyUp: {
            const auto names = key_names(ev.key_code);
            const bool down = ev.type == media::InputEventType::KeyDown;
            // Enter carries "\r" so forms submit exactly as with a physical key.
            const bool enter = ev.key_code == 0x0D;
            std::string json = std::string("{\"type\":\"") + (down ? (enter ? "keyDown" : "rawKeyDown") : "keyUp") +
                "\",\"windowsVirtualKeyCode\":" + std::to_string(ev.key_code) +
                ",\"nativeVirtualKeyCode\":" + std::to_string(ev.key_code) + ",\"modifiers\":" + std::to_string(mods);
            if (names) json += std::string(",\"key\":") + cdp::quote(names->key) + ",\"code\":" + cdp::quote(names->code);
            if (down && enter) json += ",\"text\":\"\\r\",\"unmodifiedText\":\"\\r\"";
            json += "}";
            cdp(ctx, "Input.dispatchKeyEvent", json);
            break;
        }
        case media::InputEventType::TextInput:
            cdp(ctx, "Input.insertText", "{\"text\":" + cdp::quote(ev.text) + "}");
            break;
        case media::InputEventType::FocusLost:
            // Never leave a button logically held down in the host page.
            for (std::uint8_t b = 1; b <= 5; ++b) {
                if (ctx.pressed_mask & cdp_button_mask(b)) {
                    ctx.pressed_mask &= ~cdp_button_mask(b);
                    pointer("mouseReleased", cdp_button(b), ctx.pressed_mask, 1, 0.0);
                }
            }
            break;
        }
        std::lock_guard lock(ctx.slot->mutex);
        ctx.slot->stats.inputs++;
    }

    // ---------------- Shutdown ----------------
    void release_all(std::shared_ptr<Completion<void>> done) {
        for (auto& [id, ctx] : contexts) close_context(*ctx);
        contexts.clear();
        {
            std::lock_guard lock(slots_mutex);
            slots.clear();
        }
        active.store(0);
        if (environment) {
            ComPtr<ICoreWebView2Environment5> env5;
            if (SUCCEEDED(query(environment, IID_ICoreWebView2Environment5, env5))) env5->remove_BrowserProcessExited(exit_token);
            environment.Reset();
        }
        done->set(Result<void>::success());
    }

    void cleanup() {
        bool had_thread = false;
        {
            std::lock_guard lock(thread_mutex);
            had_thread = thread_running;
        }
        HANDLE process = nullptr;
        if (had_thread) {
            const DWORD pid = browser_pid;
            if (pid != 0) process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            (void)call<void>([this](std::shared_ptr<Completion<void>> done) { release_all(done); },
                options.operation_timeout, "libération du navigateur");
        }
        guard.stop();
        bool exited = true;
        if (process) {
            exited = WaitForSingleObject(process, 8000) == WAIT_OBJECT_0;
            CloseHandle(process);
        }
        bool removed = true;
        if (!user_data_folder.empty()) {
            removed = false;
            for (int attempt = 0; attempt < 40 && !removed; ++attempt) {
                std::error_code ec;
                std::filesystem::remove_all(user_data_folder, ec);
                removed = !std::filesystem::exists(user_data_folder, ec);
                if (!removed) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (removed) user_data_folder.clear();
        }
        cleanup_complete.store(exited && removed);
        browser_pid = 0;
    }

    void shutdown() {
        cleanup();
        std::lock_guard lock(thread_mutex);
        if (thread.joinable()) {
            if (task_window) PostMessageW(task_window, WM_QUIT, 0, 0);
            // WM_QUIT posted to a window lands in the thread queue: GetMessage exits.
            thread.join();
        }
        thread_running = false;
    }

    WebView2Options options;
    EgressGuard guard;
    // thread
    std::mutex thread_mutex;
    std::thread thread;
    bool thread_running = false;
    bool thread_failed = false;
    HWND task_window = nullptr;
    std::mutex task_mutex;
    std::deque<std::function<void()>> tasks;
    // STA-owned
    ComPtr<ICoreWebView2Environment> environment;
    EventRegistrationToken exit_token{};
    std::map<ContextId, std::unique_ptr<Context>> contexts;
    std::map<ContextId, std::unique_ptr<Context>> pending_contexts;
    std::uint64_t next_context = 1;
    std::string user_data_folder;
    UINT32 browser_pid = 0;
    // shared
    std::mutex slots_mutex;
    std::map<ContextId, std::shared_ptr<FrameSlot>> slots;
    std::atomic<std::size_t> active{0};
    std::atomic<bool> browser_exited{false};
    std::atomic<bool> cleanup_complete{true};
};

bool WebView2Backend::runtime_available() {
    const Loader& l = loader();
    return l.create != nullptr && !runtime_version().empty();
}

std::string WebView2Backend::runtime_version() {
    const Loader& l = loader();
    if (!l.version) return {};
    LPWSTR version = nullptr;
    if (FAILED(l.version(nullptr, &version)) || !version) return {};
    return take_string(version);
}

bool WebView2Backend::is_available() { return kWebView2BackendImplemented && runtime_available(); }

WebView2Backend::WebView2Backend(std::string base_temp_dir)
    : WebView2Backend([&] {
          WebView2Options o;
          o.base_temp_dir = std::move(base_temp_dir);
          return o;
      }()) {}
WebView2Backend::WebView2Backend(WebView2Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
WebView2Backend::~WebView2Backend() = default;

Result<BrowserContextInfo> WebView2Backend::create_context(ParticipantId owner, const BrowserPolicy& policy) {
    if (owner.value == 0)
        return Result<BrowserContextInfo>::failure(browser_error(ErrorCode::InvalidArgument, "propriétaire de contexte invalide"));
    if (policy.allowed_origins.empty())
        return Result<BrowserContextInfo>::failure(browser_error(ErrorCode::InvalidArgument,
            "aucune origine autorisée : sélection explicite obligatoire"));
    for (const auto& origin : policy.allowed_origins) {
        if (!url_origin::canonicalize(origin, true))
            return Result<BrowserContextInfo>::failure(browser_error(ErrorCode::InvalidArgument,
                "origine autorisée invalide (http(s)://hôte[:port] exact attendu)"));
    }
    if (!runtime_available())
        return Result<BrowserContextInfo>::failure(browser_error(ErrorCode::PlatformError,
            "runtime WebView2 Evergreen ou WebView2Loader.dll absent"));
    return impl_->call<BrowserContextInfo>(
        [this, owner, policy](std::shared_ptr<Completion<BrowserContextInfo>> done) {
            impl_->start_create(done, owner, policy);
        },
        impl_->options.operation_timeout, "création du contexte navigateur");
}

Result<void> WebView2Backend::navigate(ContextId ctx, const std::string& url) {
    return impl_->call<void>(
        [this, ctx, url](std::shared_ptr<Completion<void>> done) {
            auto* c = impl_->find(ctx);
            if (!c) { done->set(Result<void>::failure(browser_error(ErrorCode::NotFound, "contexte introuvable"))); return; }
            if (!c->policy.is_origin_allowed(url)) {
                impl_->count(*c, &BrowserContextStats::blocked_navigations);
                done->set(Result<void>::failure(browser_error(ErrorCode::PermissionDenied,
                    "navigation refusée par la politique d'isolation")));
                return;
            }
            if (c->navigation) c->navigation->set(Result<void>::failure(browser_error(ErrorCode::Cancelled, "remplacée")));
            c->navigation = done;
            c->navigation_blocked = false;
            const std::wstring wide = to_wide(url);
            const HRESULT hr = c->webview->Navigate(wide.c_str());
            if (FAILED(hr)) {
                c->navigation.reset();
                done->set(Result<void>::failure(browser_error(ErrorCode::InvalidArgument, "URL refusée par WebView2", hr)));
            }
        },
        impl_->options.operation_timeout, "navigation");
}

Result<void> WebView2Backend::destroy_context(ContextId ctx) {
    if (!impl_->slot_of(ctx)) return Result<void>::success(); // idempotent
    return impl_->call<void>(
        [this, ctx](std::shared_ptr<Completion<void>> done) {
            auto it = impl_->contexts.find(ctx);
            if (it != impl_->contexts.end()) {
                impl_->close_context(*it->second);
                impl_->contexts.erase(it);
            }
            {
                std::lock_guard lock(impl_->slots_mutex);
                impl_->slots.erase(ctx);
            }
            impl_->active.store(impl_->contexts.size());
            done->set(Result<void>::success());
        },
        impl_->options.operation_timeout, "destruction du contexte");
}

Result<std::shared_ptr<const CapturedFrame>> WebView2Backend::wait_frame(ContextId ctx, std::uint64_t after_sequence,
    std::chrono::milliseconds timeout) {
    using R = Result<std::shared_ptr<const CapturedFrame>>;
    auto slot = impl_->slot_of(ctx);
    if (!slot) return R::failure(browser_error(ErrorCode::NotFound, "contexte introuvable"));
    std::unique_lock lock(slot->mutex);
    slot->cv.wait_for(lock, timeout, [&] {
        return slot->closed || (slot->latest && slot->latest->sequence > after_sequence);
    });
    if (slot->closed) return R::failure(browser_error(ErrorCode::Cancelled, "contexte fermé"));
    if (!slot->latest || slot->latest->sequence <= after_sequence)
        return R::failure(browser_error(ErrorCode::TimedOut, "aucune nouvelle frame"));
    return R::success(slot->latest);
}

Result<std::vector<std::uint8_t>> WebView2Backend::capture_frame(ContextId ctx) {
    auto frame = wait_frame(ctx, 0, std::chrono::milliseconds(0));
    if (!frame) return Result<std::vector<std::uint8_t>>::failure(frame.error());
    return Result<std::vector<std::uint8_t>>::success(frame.value()->data);
}

Result<void> WebView2Backend::inject_input(ContextId ctx, const std::vector<std::uint8_t>& event) {
    auto decoded = media::deserialize_input_event(event);
    if (!decoded) return Result<void>::failure(decoded.error());
    if (!impl_->slot_of(ctx)) return Result<void>::failure(browser_error(ErrorCode::NotFound, "contexte introuvable"));
    // Posted in order to the STA: ordering is preserved without waiting for
    // Chromium's acknowledgement on the hot input path.
    auto ev = std::move(decoded).value();
    if (!impl_->post([this, ctx, ev]() {
            if (auto* c = impl_->find(ctx)) impl_->inject(*c, ev);
        }))
        return Result<void>::failure(browser_error(ErrorCode::PlatformError, "thread navigateur indisponible"));
    return Result<void>::success();
}

Result<std::string> WebView2Backend::evaluate_for_diagnostics(ContextId ctx, const std::string& script) {
    return impl_->call<std::string>(
        [this, ctx, script](std::shared_ptr<Completion<std::string>> done) {
            auto* c = impl_->find(ctx);
            if (!c || !c->webview) {
                done->set(Result<std::string>::failure(browser_error(ErrorCode::NotFound, "contexte introuvable")));
                return;
            }
            const std::wstring wide = to_wide(script);
            auto handler = make_callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                IID_ICoreWebView2ExecuteScriptCompletedHandler, [done](HRESULT hr, LPCWSTR json) -> HRESULT {
                    if (FAILED(hr)) done->set(Result<std::string>::failure(browser_error(ErrorCode::PlatformError, "script refusé", hr)));
                    else done->set(Result<std::string>::success(to_utf8(json)));
                    return S_OK;
                });
            const HRESULT hr = c->webview->ExecuteScript(wide.c_str(), handler.Get());
            if (FAILED(hr)) done->set(Result<std::string>::failure(browser_error(ErrorCode::PlatformError, "script refusé", hr)));
        },
        impl_->options.operation_timeout, "évaluation de diagnostic");
}

Result<BrowserContextStats> WebView2Backend::context_stats(ContextId ctx) {
    auto slot = impl_->slot_of(ctx);
    if (!slot) return Result<BrowserContextStats>::failure(browser_error(ErrorCode::NotFound, "contexte introuvable"));
    std::lock_guard lock(slot->mutex);
    return Result<BrowserContextStats>::success(slot->stats);
}

std::size_t WebView2Backend::active_contexts() const { return impl_->active.load(); }
EgressCounters WebView2Backend::egress_counters() const { return impl_->guard.counters(); }
void WebView2Backend::cleanup_all() { impl_->cleanup(); }
std::uint32_t WebView2Backend::browser_process_id() const { return impl_->browser_pid; }
bool WebView2Backend::last_cleanup_complete() const { return impl_->cleanup_complete.load(); }
#endif // DEVLINK_HAVE_WEBVIEW2

// This factory is reserved for the synthetic demo/test backend. The desktop
// never uses it to claim a real engine.
std::unique_ptr<IRemoteBrowserBackend> create_demo_browser_backend(const std::string& custom_temp_dir) {
    return std::make_unique<DemoBrowserBackend>(custom_temp_dir);
}

} // namespace plpt::browser
