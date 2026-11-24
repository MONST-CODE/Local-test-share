// DevLink — tests/integration/browser_pipeline_tests.cpp
// REAL browser qualification (Windows, WebView2). No mock, no synthetic frame:
// the real devlink-demo-service serves /fixture, a real Chromium renders it,
// its compositor frames are decoded with the OS JPEG codec and the run id is
// read back FROM THE PIXELS. Input is injected through the backend and checked
// both in the DOM (host side) and in later frames.
// Exit 77 = SKIP (no WebView2 runtime on this machine): never counted as PASS.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "tests/test_harness.hpp"
#include "tests/integration/fixture_support.hpp"
#include "engine/browser/cdp_json.hpp"
#include "engine/browser/remote_frame.hpp"
#include "engine/browser/webview2_backend.hpp"
#include "engine/media/jpeg_decoder.hpp"

using namespace plpt;
namespace cdp = plpt::browser::cdp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

#if defined(_WIN32)
plpt_fixture::FixtureService* g_service = nullptr;

BrowserPolicy fixture_policy() {
    BrowserPolicy policy;
    policy.allowed_origins.push_back(g_service->origin());
    return policy;
}

struct Seen {
    std::shared_ptr<const CapturedFrame> frame;
    media::DecodedImage image;
};

template <class Pred>
std::optional<Seen> wait_for_pixels(browser::WebView2Backend& backend, ContextId ctx, Pred pred,
    std::chrono::milliseconds timeout, std::uint64_t after = 0) {
    media::JpegDecoder decoder;
    const auto deadline = Clock::now() + timeout;
    std::uint64_t last = after;
    while (Clock::now() < deadline) {
        auto f = backend.wait_frame(ctx, last, 250ms);
        if (!f) continue;
        last = f.value()->sequence;
        Seen seen;
        seen.frame = f.value();
        if (!decoder.decode(seen.frame->data, seen.frame->width, seen.frame->height, seen.image)) continue;
        if (pred(seen.image)) return seen;
    }
    return std::nullopt;
}

std::optional<cdp::Value> fixture_state(browser::WebView2Backend& backend, ContextId ctx) {
    auto raw = backend.evaluate_for_diagnostics(ctx, "window.devlinkFixture ? window.devlinkFixture() : null");
    if (!raw) return std::nullopt;
    auto outer = cdp::parse(raw.value());
    if (!outer || outer->kind != cdp::Value::Kind::String) return std::nullopt;
    return cdp::parse(outer->text);
}

std::vector<std::uint8_t> input(media::InputEventType type, float x = 0, float y = 0, std::uint8_t button = 0,
    std::uint32_t key = 0, std::string text = {}) {
    media::InputEvent ev;
    ev.type = type;
    ev.x_norm = x;
    ev.y_norm = y;
    ev.button = button;
    ev.key_code = key;
    ev.text = std::move(text);
    auto bytes = media::serialize_input_event(ev);
    return bytes ? bytes.value() : std::vector<std::uint8_t>{};
}

void click(browser::WebView2Backend& backend, ContextId ctx, float x, float y) {
    (void)backend.inject_input(ctx, input(media::InputEventType::MouseMove, x, y));
    (void)backend.inject_input(ctx, input(media::InputEventType::MouseDown, x, y, 1));
    (void)backend.inject_input(ctx, input(media::InputEventType::MouseUp, x, y, 1));
}

// Button centre (450,150) and input centre (500,280) in the 1280x720 viewport.
constexpr float kButtonX = 450.0f / 1280.0f, kButtonY = 150.0f / 720.0f;
constexpr float kFieldX = 500.0f / 1280.0f, kFieldY = 280.0f / 720.0f;

void real_browser_renders_fixture_pixels() {
    browser::WebView2Backend backend;
    const auto t0 = Clock::now();
    auto ctx = backend.create_context(ParticipantId{1}, fixture_policy());
    REQUIRE(ctx);
    const auto t_ctx = Clock::now();
    REQUIRE(backend.navigate(ctx.value().context, g_service->origin() + "/fixture"));
    const auto t_nav = Clock::now();
    const auto run = g_service->run_id();
    auto seen = wait_for_pixels(backend, ctx.value().context,
        [&](const media::DecodedImage& img) { return plpt_fixture::read_run_band(img) == run; }, 15s);
    REQUIRE(seen);
    const auto t_frame = Clock::now();
    CHECK_EQ(seen->image.width, 1280U);
    CHECK_EQ(seen->image.height, 720U);
    CHECK_EQ(plpt_fixture::square_colour(seen->image), 0);
    auto state = fixture_state(backend, ctx.value().context);
    REQUIRE(state);
    CHECK(state->string_at("run") == std::optional<std::string_view>(run));
    auto stats = backend.context_stats(ctx.value().context);
    REQUIRE(stats);
    CHECK(stats.value().navigated);
    CHECK_EQ(stats.value().last_http_status, 200);
    std::printf("EVIDENCE run_id=%s read_from_pixels=%s frame_bytes=%zu %ux%u\n", run.c_str(),
        plpt_fixture::read_run_band(seen->image).c_str(), seen->frame->data.size(), seen->frame->width, seen->frame->height);
    std::printf("METRIC startup_context_ms=%lld navigate_ms=%lld first_marked_frame_ms=%lld\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t_ctx - t0).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t_nav - t_ctx).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t_frame - t_nav).count()));

    // Sustained frame rate while the page's JS ticker animates (100 ms) — the
    // compositor only emits frames when pixels change.
    std::uint64_t last = seen->frame->sequence;
    unsigned frames = 0;
    std::size_t bytes = 0;
    const auto start = Clock::now();
    while (Clock::now() - start < 3s) {
        auto f = backend.wait_frame(ctx.value().context, last, 500ms);
        if (!f) continue;
        last = f.value()->sequence;
        ++frames;
        bytes += f.value()->data.size();
    }
    std::printf("METRIC ticker_page_fps=%.1f avg_frame_kb=%.1f\n", frames / 3.0, frames ? bytes / 1024.0 / frames : 0.0);
    CHECK(frames >= 15); // ticker changes 10x/s at minimum
    REQUIRE(backend.destroy_context(ctx.value().context));
    CHECK(backend.destroy_context(ctx.value().context)); // idempotent
    CHECK_EQ(backend.active_contexts(), 0U);
}

void real_click_and_unicode_text_change_dom_and_pixels() {
    browser::WebView2Backend backend;
    auto ctx = backend.create_context(ParticipantId{2}, fixture_policy());
    REQUIRE(ctx);
    const auto id = ctx.value().context;
    REQUIRE(backend.navigate(id, g_service->origin() + "/fixture"));
    auto first = wait_for_pixels(backend, id, [](const media::DecodedImage& img) { return plpt_fixture::square_colour(img) == 0; }, 15s);
    REQUIRE(first);
    // Click -> square turns green in later frames AND clicks==1 in the DOM.
    const auto t_click = Clock::now();
    click(backend, id, kButtonX, kButtonY);
    auto green = wait_for_pixels(backend, id, [](const media::DecodedImage& img) { return plpt_fixture::square_colour(img) == 1; },
        5s, first->frame->sequence);
    REQUIRE(green);
    const auto click_to_pixels = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_click).count();
    auto state = fixture_state(backend, id);
    REQUIRE(state);
    CHECK(state->number_at("clicks") == std::optional<double>(1.0));
    std::printf("METRIC click_to_changed_frame_ms=%lld (inject -> host DOM -> compositor -> decoded)\n",
        static_cast<long long>(click_to_pixels));
    // Second click -> blue.
    click(backend, id, kButtonX, kButtonY);
    CHECK(wait_for_pixels(backend, id, [](const media::DecodedImage& img) { return plpt_fixture::square_colour(img) == 2; },
        5s, green->frame->sequence));
    // Focus the field, type Unicode text, then Backspace and Enter keys.
    click(backend, id, kFieldX, kFieldY);
    const std::string typed = "héllo ✓ DevLink";
    CHECK(backend.inject_input(id, input(media::InputEventType::TextInput, 0, 0, 0, 0, typed)));
    CHECK(backend.inject_input(id, input(media::InputEventType::KeyDown, 0, 0, 0, 0x08)));
    CHECK(backend.inject_input(id, input(media::InputEventType::KeyUp, 0, 0, 0, 0x08)));
    std::this_thread::sleep_for(400ms);
    state = fixture_state(backend, id);
    REQUIRE(state);
    CHECK(state->string_at("value") == std::optional<std::string_view>("héllo ✓ DevLin"));
    CHECK(state->string_at("focused") == std::optional<std::string_view>("field"));
    CHECK(state->number_at("keys").value_or(0) >= 1.0);
    CHECK(state->number_at("clicks") == std::optional<double>(2.0));
    // Malformed / hostile input bytes are rejected before reaching the browser.
    auto garbage = input(media::InputEventType::MouseMove, 0.5f, 0.5f);
    garbage[0] ^= 0xFF;
    CHECK(!backend.inject_input(id, garbage));
    CHECK(!backend.inject_input(id, {}));
    CHECK(!backend.inject_input(ContextId{999}, input(media::InputEventType::MouseMove, 0.5f, 0.5f)));
}

void mode_a_profiles_are_isolated() {
    browser::WebView2Backend backend;
    auto a = backend.create_context(ParticipantId{10}, fixture_policy());
    auto b = backend.create_context(ParticipantId{11}, fixture_policy());
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a.value().profile_id != b.value().profile_id);
    REQUIRE(backend.navigate(a.value().context, g_service->origin() + "/fixture"));
    REQUIRE(backend.navigate(b.value().context, g_service->origin() + "/fixture"));
    REQUIRE(backend.evaluate_for_diagnostics(a.value().context,
        "localStorage.setItem('devlink-owner','A');sessionStorage.setItem('devlink-owner','A');"
        "document.cookie='devlink=A; path=/';1"));
    // Reload B: if storage leaked across profiles it would now see A's values.
    REQUIRE(backend.navigate(b.value().context, g_service->origin() + "/fixture"));
    auto sa = fixture_state(backend, a.value().context);
    auto sb = fixture_state(backend, b.value().context);
    REQUIRE(sa);
    REQUIRE(sb);
    CHECK(sa->string_at("owner") == std::optional<std::string_view>("A"));
    CHECK(sa->string_at("cookie") == std::optional<std::string_view>("devlink=A"));
    const auto* owner_b = sb->get("owner");
    CHECK(owner_b && owner_b->kind == cdp::Value::Kind::Null);
    CHECK(sb->string_at("cookie") == std::optional<std::string_view>(""));
    // Input to A never reaches B.
    click(backend, a.value().context, kButtonX, kButtonY);
    std::this_thread::sleep_for(500ms);
    sa = fixture_state(backend, a.value().context);
    sb = fixture_state(backend, b.value().context);
    REQUIRE(sa);
    REQUIRE(sb);
    CHECK(sa->number_at("clicks") == std::optional<double>(1.0));
    CHECK(sb->number_at("clicks") == std::optional<double>(0.0));
}

void origin_policy_blocks_everything_else() {
    plpt_fixture::Canary canary;
    REQUIRE(canary.start());
    const std::string forbidden = "http://127.0.0.1:" + std::to_string(canary.port());
    browser::WebView2Backend backend;
    // No explicit selection -> no context at all.
    CHECK(!backend.create_context(ParticipantId{20}, BrowserPolicy{}));
    BrowserPolicy wildcard;
    wildcard.allowed_origins.push_back("http://*");
    CHECK(!backend.create_context(ParticipantId{20}, wildcard));
    auto ctx = backend.create_context(ParticipantId{20}, fixture_policy());
    REQUIRE(ctx);
    const auto id = ctx.value().context;
    REQUIRE(backend.navigate(id, g_service->origin() + "/fixture"));
    for (const char* target : {"file:///C:/Windows/win.ini", "javascript:alert(1)", "about:config", "ms-settings:display",
             "http://127.0.0.1:5432/", "http://localhost:2375/version", "http://[::1]:9/"}) {
        auto r = backend.navigate(id, target);
        CHECK(!r);
        if (!r) CHECK_EQ(r.error().code, ErrorCode::PermissionDenied);
    }
    CHECK(!backend.navigate(id, forbidden + "/"));
    // Redirect served by the allowed origin towards a forbidden port.
    auto redirected = backend.navigate(id, g_service->origin() + "/redirect-out");
    CHECK(!redirected);
    if (!redirected) CHECK_EQ(redirected.error().code, ErrorCode::PermissionDenied);
    REQUIRE(backend.navigate(id, g_service->origin() + "/fixture"));
    // The PAGE tries: script navigation, fetch, image, iframe, popup, download.
    const std::string attempts =
        "location.href='" + forbidden + "/nav';"
        "fetch('" + forbidden + "/fetch').catch(()=>{});"
        "fetch('http://localhost:" + std::to_string(canary.port()) + "/alias').catch(()=>{});"
        "(new Image()).src='" + forbidden + "/img.png';"
        "var f=document.createElement('iframe');f.src='" + forbidden + "/frame';document.body.appendChild(f);"
        "window.open('" + forbidden + "/popup');window.open('" + g_service->origin() + "/fixture');"
        "var a=document.createElement('a');a.href='/fixture.css';a.download='x.css';document.body.appendChild(a);a.click();1";
    CHECK(backend.evaluate_for_diagnostics(id, attempts));
    std::this_thread::sleep_for(1500ms);
    auto stats = backend.context_stats(id);
    REQUIRE(stats);
    CHECK_EQ(canary.hits(), 0U);
    CHECK(stats.value().blocked_requests >= 2);
    CHECK(stats.value().blocked_popups >= 2);
    CHECK(stats.value().blocked_navigations >= 1);
    CHECK(stats.value().current_url.rfind(g_service->origin(), 0) == 0);
    auto state = fixture_state(backend, id);
    CHECK(state.has_value()); // still on the fixture, not navigated away
    const auto egress = backend.egress_counters();
    CHECK(egress.denied >= 1);
    CHECK(egress.allowed >= 1);
    std::printf("EVIDENCE egress_allowed=%llu egress_denied=%llu recent_denied=%s\n",
        static_cast<unsigned long long>(egress.allowed), static_cast<unsigned long long>(egress.denied),
        egress.recent_denied.empty() ? "-" : egress.recent_denied.back().c_str());
    std::printf("EVIDENCE canary_hits=%u blocked_requests=%llu blocked_popups=%llu blocked_navigations=%llu blocked_downloads=%llu\n",
        canary.hits(), static_cast<unsigned long long>(stats.value().blocked_requests),
        static_cast<unsigned long long>(stats.value().blocked_popups),
        static_cast<unsigned long long>(stats.value().blocked_navigations),
        static_cast<unsigned long long>(stats.value().blocked_downloads));
}

void lifecycle_cleanup_is_complete_and_repeatable() {
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::uint32_t pid = 0;
        browser::WebView2Backend backend;
        for (std::uint64_t p = 1; p <= 3; ++p) {
            auto ctx = backend.create_context(ParticipantId{100 + p}, fixture_policy());
            REQUIRE(ctx);
            REQUIRE(backend.navigate(ctx.value().context, g_service->origin() + "/fixture"));
        }
        CHECK_EQ(backend.active_contexts(), 3U);
        pid = backend.browser_process_id();
        CHECK(pid != 0);
        backend.cleanup_all();
        CHECK_EQ(backend.active_contexts(), 0U);
        CHECK(backend.last_cleanup_complete());
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        const bool gone = process == nullptr || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (process) CloseHandle(process);
        CHECK(gone);
        backend.cleanup_all(); // idempotent
    }
}
#endif

} // namespace

int main() {
#if defined(_WIN32)
    if (!browser::WebView2Backend::is_available()) {
        std::printf("SKIP browser: runtime WebView2 Evergreen ou WebView2Loader.dll absent\n");
        return 77;
    }
    std::printf("WebView2 runtime %s\n", browser::WebView2Backend::runtime_version().c_str());
    plpt_fixture::FixtureService service;
    if (!service.start(plpt_fixture::random_run_id())) {
        std::printf("FAIL devlink-demo-service introuvable ou muet\n");
        return 1;
    }
    g_service = &service;
    CASE(real_browser_renders_fixture_pixels);
    CASE(real_click_and_unicode_text_change_dom_and_pixels);
    CASE(mode_a_profiles_are_isolated);
    CASE(origin_policy_blocks_everything_else);
    CASE(lifecycle_cleanup_is_complete_and_repeatable);
    service.stop();
    return plpt_test::finish("browser");
#else
    std::printf("SKIP browser: WebView2 Browser Worker is Windows-only (ADR-018)\n");
    return 77;
#endif
}
