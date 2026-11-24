// DevLink — tests/integration/desktop_e2e_tests.cpp
// End-to-end qualification of the SHIPPED desktop application, not of an
// engine harness: three real processes on this machine.
//
//   devlink-demo-service  : the local web application under test (/fixture)
//   devlink-desktop --host: real WebView2 Browser Worker, control plane, media
//   devlink-desktop --join: real SDL3 window; synthetic SDL mouse/text events
//                           go through ImGui, the letterbox mapping, the TLS 1.3
//                           media channel and the host SharePolicy.
//
// Proof points: the guest receives real frames over TLS 1.3; its clicks and
// Unicode text change the HOST DOM (read host-side); the guest's own window
// capture contains the fixture square in the colour of a click count the host
// really reached; both processes exit 0 and every browser context is destroyed.
// It opens real, focused windows and moves the real cursor: do not use the
// mouse while it runs (outside clicks are detected and reported as evidence).
// Loopback, single machine: NOT a LAN qualification. Exit 77 = SKIP.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "tests/test_harness.hpp"
#include "tests/integration/fixture_support.hpp"
#include "engine/browser/cdp_json.hpp"
#include "engine/browser/webview2_backend.hpp"
#include "engine/net/transport.hpp"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
#if defined(_WIN32)
namespace cdp = plpt::browser::cdp;

// A child process whose stdout lines are collected by a reader thread.
class Child {
public:
    ~Child() { kill(); }
    bool start(const std::wstring& args) {
        const auto exe = plpt_fixture::executable_dir() / "devlink-desktop.exe";
        if (!std::filesystem::exists(exe)) return false;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE write = nullptr;
        if (!CreatePipe(&read_, &write, &sa, 0)) return false;
        SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write;
        si.hStdError = write;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(write);
        if (!ok) return false;
        process_ = pi.hProcess;
        CloseHandle(pi.hThread);
        reader_ = std::thread([this] {
            std::string line;
            char buf[512];
            DWORD got = 0;
            while (ReadFile(read_, buf, sizeof buf, &got, nullptr) && got > 0) {
                for (DWORD i = 0; i < got; ++i) {
                    if (buf[i] == '\n') {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        std::lock_guard l(mutex_);
                        lines_.push_back(std::move(line));
                        line.clear();
                    } else {
                        line.push_back(buf[i]);
                    }
                }
            }
        });
        return true;
    }
    // First line (from index `from`) matching re, waiting up to timeout.
    std::optional<std::smatch> wait_line(const std::regex& re, std::chrono::milliseconds timeout, std::string* storage) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            {
                std::lock_guard l(mutex_);
                for (const auto& line : lines_) {
                    std::smatch m;
                    if (std::regex_search(line, m, re)) {
                        *storage = line;
                        std::smatch again;
                        std::regex_search(*storage, again, re);
                        return again;
                    }
                }
            }
            std::this_thread::sleep_for(50ms);
        }
        return std::nullopt;
    }
    // Last line matching re satisfying pred.
    template <class P> bool wait_any(const std::regex& re, P pred, std::chrono::milliseconds timeout) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            {
                std::lock_guard l(mutex_);
                for (const auto& line : lines_) {
                    std::smatch m;
                    if (std::regex_search(line, m, re) && pred(m)) return true;
                }
            }
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }
    int wait_exit(std::chrono::milliseconds timeout) {
        if (!process_) return -1;
        if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return -2;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return static_cast<int>(code);
    }
    // Lines safe to print: the invite line is never echoed.
    void dump(const char* who) {
        std::lock_guard l(mutex_);
        for (const auto& line : lines_)
            if (line.find("invite=") == std::string::npos) std::printf("  [%s] %s\n", who, line.c_str());
    }
    void kill() {
        if (process_) {
            if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) TerminateProcess(process_, 99);
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (reader_.joinable()) reader_.join();
        if (read_) { CloseHandle(read_); read_ = nullptr; }
    }

private:
    HANDLE process_ = nullptr;
    HANDLE read_ = nullptr;
    std::thread reader_;
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Fixture square (demo-service /fixture): colour = clicks mod 4.
struct SquareColour { int r, g, b; const char* name; };
constexpr SquareColour kSquareColours[4] = {
    {0xd0, 0x10, 0x10, "red"}, {0x10, 0xb0, 0x10, "green"}, {0x10, 0x30, 0xd0, "blue"}, {0xe0, 0xd0, 0x10, "yellow"}};

// Counts, in a 24/32-bit BMP, the pixels close to each fixture square colour
// (JPEG and scaling blur the edges, hence the per-channel tolerance).
bool square_pixels_in_bmp(const std::filesystem::path& path, int* width, int* height, long long counts[4]) {
    for (int i = 0; i < 4; ++i) counts[i] = 0;
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    auto u32 = [&](std::size_t o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<unsigned>(d[o + 3]) << 24); };
    const std::size_t offset = u32(10);
    const int w = static_cast<int>(u32(18));
    const int h_raw = static_cast<int>(u32(22));
    const int bpp = d[28] | (d[29] << 8);
    const int h = h_raw < 0 ? -h_raw : h_raw;
    *width = w;
    *height = h;
    if (bpp != 24 && bpp != 32) return false;
    const std::size_t bytes = static_cast<std::size_t>(bpp / 8);
    const std::size_t stride = (static_cast<std::size_t>(w) * bytes + 3) & ~static_cast<std::size_t>(3);
    constexpr int kTolerance = 45;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t o = offset + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * bytes;
            if (o + 2 >= d.size()) return true;
            const int b = d[o], g = d[o + 1], r = d[o + 2];
            for (int i = 0; i < 4; ++i) {
                const auto& c = kSquareColours[i];
                if (std::abs(r - c.r) <= kTolerance && std::abs(g - c.g) <= kTolerance && std::abs(b - c.b) <= kTolerance) ++counts[i];
            }
        }
    return true;
}

std::optional<cdp::Value> fixture_state(const std::string& line_json) {
    auto outer = cdp::parse(line_json);
    if (!outer || outer->kind != cdp::Value::Kind::String) return std::nullopt;
    return cdp::parse(outer->text);
}

plpt_fixture::FixtureService* g_service = nullptr;

void shipped_desktop_host_and_guest_share_a_real_application() {
    const auto dir = plpt_fixture::executable_dir();
    const auto capture = dir / "e2e-guest-window.bmp";
    std::error_code ec;
    std::filesystem::remove(capture, ec);
    const std::string typed = "DevLink é✓";

    Child host;
    REQUIRE(host.start(widen("--host " + g_service->origin() + " --entry " + g_service->origin() +
        "/fixture --mode B --bind 127.0.0.1 --no-approval --print-invite --report-fixture "
        "--exit-when-guests-gone --exit-after 120")));
    std::string active_line;
    auto active = host.wait_line(std::regex(R"(devlink-host: active invite=(\S+) control=([0-9.]+):(\d+))"), 40s, &active_line);
    if (!active) host.dump("host");
    REQUIRE(active);
    const std::string code = (*active)[1];
    const std::string port = (*active)[3];
    std::printf("EVIDENCE host_active control_port=%s (invite read over a pipe, never printed)\n", port.c_str());

    Child guest;
    // Button centre (450,150) then text field (500,280) in the 1280x720 viewport.
    REQUIRE(guest.start(widen("--join 127.0.0.1 " + port + " --code " + code + " --name E2E --request-control "
        "--auto-click 0.3516,0.2083 --auto-click 0.3906,0.3889 --auto-type \"" + typed + "\" "
        "--capture-after 14 \"" + capture.string() + "\" --exit-after 17")));

    const std::regex guest_re(R"(devlink-guest: phase=(.+?) frames=(\d+) fps=([0-9.]+) transport=(\S+).*? mode=(\d) may_input=(\d))");
    const bool streaming = guest.wait_any(guest_re, [](const std::smatch& m) {
        return std::stoull(m[2]) > 0 && m[4].str().rfind("TLSv1.3", 0) == 0;
    }, 20s);
    CHECK(streaming);
    CHECK(guest.wait_any(guest_re, [](const std::smatch& m) { return m[6] == "1"; }, 15s)); // control granted

    // HOST DOM: the click and the Unicode text came through the guest window.
    bool clicked = false, text_ok = false;
    const auto until = Clock::now() + 20s;
    while (Clock::now() < until && !(clicked && text_ok)) {
        host.wait_any(std::regex(R"(devlink-host-fixture: (.*)$)"), [&](const std::smatch& m) {
            auto st = fixture_state(m[1]);
            if (!st) return false;
            if (st->number_at("clicks").value_or(0) >= 1) clicked = true;
            if (st->string_at("value") == std::optional<std::string_view>(typed)) text_ok = true;
            return clicked && text_ok;
        }, 500ms);
    }
    CHECK(clicked);
    CHECK(text_ok);
    std::printf("EVIDENCE host_dom clicks>=1=%d value_matches_guest_typing=%d\n", clicked ? 1 : 0, text_ok ? 1 : 0);

    // The guest's REAL window shows the host page with the square in the clicked colour.
    const int guest_rc = guest.wait_exit(40s);
    CHECK_EQ(guest_rc, 0);
    // Etat de la fenetre invitee au moment de la capture (normal, minimized...)
    // et nombre de trames sans surface d'affichage sautees : trace de la preuve.
    std::string capture_line;
    const auto capture_state = guest.wait_line(
        std::regex(R"(devlink-capture: .* saved window=(\S+) deferred_frames=(\d+))"), 2s, &capture_line);
    CHECK(capture_state.has_value());
    if (capture_state)
        std::printf("EVIDENCE guest_capture_window=%s deferred_frames=%s\n",
            (*capture_state)[1].str().c_str(), (*capture_state)[2].str().c_str());
    REQUIRE(std::filesystem::exists(capture));
    // Every click count the HOST really reached (read from its DOM reports).
    // This test runs on a live desktop: real clicks made while the guest window
    // has the focus also reach the page. The capture must show the square in the
    // colour of a state the host reached after at least one click (clicks mod 4);
    // with no outside input that is exactly one click, i.e. green.
    std::set<long long> host_clicks;
    host.wait_any(std::regex(R"(devlink-host-fixture: (.*)$)"), [&](const std::smatch& m) {
        if (auto st = fixture_state(m[1]))
            host_clicks.insert(static_cast<long long>(st->number_at("clicks").value_or(0)));
        return false;
    }, 200ms);
    long long counts[4] = {0, 0, 0, 0};
    int w = 0, h = 0;
    REQUIRE(square_pixels_in_bmp(capture, &w, &h, counts));
    int shown = 0;
    for (int i = 1; i < 4; ++i)
        if (counts[i] > counts[shown]) shown = i;
    bool state_reached = false;
    for (const long long n : host_clicks) state_reached = state_reached || (n >= 1 && n % 4 == shown);
    const long long max_clicks = host_clicks.empty() ? 0 : *host_clicks.rbegin();
    std::printf("EVIDENCE guest_window_capture=%dx%d square=%s pixels=%lld host_clicks_max=%lld file=%s\n", w, h,
        kSquareColours[shown].name, counts[shown], max_clicks, capture.filename().string().c_str());
    if (max_clicks > 1)
        std::printf("EVIDENCE external_input clicks_max=%lld (one scripted click: the desktop was used during the test)\n",
            max_clicks);
    const bool square_ok = counts[shown] > 2000 && state_reached;
    CHECK(counts[shown] > 2000);   // the host page is really drawn in the guest's own window
    CHECK(state_reached);          // and it shows a state the host really reached after a click

    // The guest left: the host stops by itself and destroys every context.
    const int host_rc = host.wait_exit(40s);
    CHECK_EQ(host_rc, 0);
    std::string stopped;
    auto end = host.wait_line(std::regex(R"(devlink-host: stopped stage=.* contexts=(\d+)/(\d+))"), 5s, &stopped);
    REQUIRE(end);
    CHECK((*end)[1] == (*end)[2]);
    if (!streaming || !clicked || !text_ok || !square_ok) {
        host.dump("host");
        guest.dump("guest");
    }
}
#endif
} // namespace

int main() {
#if defined(_WIN32)
    if (!plpt::browser::WebView2Backend::is_available()) {
        std::printf("SKIP desktop-e2e: runtime WebView2 absent\n");
        return 77;
    }
    if (!plpt::net::tls13_runtime_available()) {
        std::printf("SKIP desktop-e2e: runtime TLS 1.3 absent\n");
        return 77;
    }
    plpt_fixture::FixtureService service;
    if (!service.start(plpt_fixture::random_run_id())) {
        std::printf("FAIL devlink-demo-service introuvable\n");
        return 1;
    }
    g_service = &service;
    CASE(shipped_desktop_host_and_guest_share_a_real_application);
    service.stop();
    return plpt_test::finish("desktop-e2e");
#else
    std::printf("SKIP desktop-e2e: Browser Worker Windows-only (ADR-018)\n");
    return 77;
#endif
}
