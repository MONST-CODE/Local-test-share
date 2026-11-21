// DevLink — tests/integration/share_pipeline_tests.cpp
// End-to-end REAL sharing pipeline on one machine (loopback). Everything is the
// production code path: devlink-demo-service fixture, WebView2 Browser Worker,
// control plane with challenge-response + TLS 1.3, MediaOffer, separate TLS 1.3
// PSK media channel, WIC decode on the guest, input records authorised by the
// host SharePolicy. Loopback is NOT a LAN qualification (see
// docs/reports/REAL_LAN_QUALIFICATION.md); it proves the pipeline is complete.
// Exit 77 = SKIP (no WebView2 runtime / no TLS runtime): never a PASS.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "tests/test_harness.hpp"
#include "tests/integration/fixture_support.hpp"
#include "engine/browser/cdp_json.hpp"
#include "engine/browser/webview2_backend.hpp"
#include "engine/core/clock.hpp"
#include "engine/core/secrets.hpp"
#include "engine/net/guest_client.hpp"
#include "engine/net/host_server.hpp"
#include "engine/net/transport.hpp"
#include "engine/share/share_guest.hpp"
#include "engine/share/share_host.hpp"

using namespace plpt;
using namespace plpt::net;
using namespace plpt::share;
using namespace std::chrono_literals;
namespace cdp = plpt::browser::cdp;
using Clock = std::chrono::steady_clock;

namespace {
#if defined(_WIN32)
plpt_fixture::FixtureService* g_service = nullptr;

constexpr float kButtonX = 450.0f / 1280.0f, kButtonY = 150.0f / 720.0f;
constexpr float kFieldX = 500.0f / 1280.0f, kFieldY = 280.0f / 720.0f;

media::InputEvent ev(media::InputEventType type, float x = 0, float y = 0, std::uint8_t button = 0) {
    media::InputEvent e;
    e.type = type;
    e.x_norm = x;
    e.y_norm = y;
    e.button = button;
    return e;
}

struct Host {
    SystemClock clock;
    std::unique_ptr<ShareHost> share;
    std::unique_ptr<HostServer> server;
    std::uint16_t port = 0;

    bool start(ShareMode mode, std::size_t max_participants = 5) {
        share = std::make_unique<ShareHost>(std::make_unique<browser::WebView2Backend>());
        ShareHostConfig sc;
        sc.mode = mode;
        sc.allowed_origins = {g_service->origin()};
        sc.entry_url = g_service->origin() + "/fixture";
        sc.bind_address = IpAddress::loopback(IpFamily::Ipv4);
        auto ok = share->start(sc);
        if (!ok) { std::printf("share start: %s\n", ok.error().message.c_str()); return false; }
        HostConfig hc;
        hc.session_name = "Qualification DevLink";
        hc.host_name = "hôte";
        hc.bind_address = IpAddress::loopback(IpFamily::Ipv4);
        hc.max_participants = max_participants;
        hc.approval_required = false;
        hc.tls_required = true;
        hc.heartbeat_interval_ms = 500;
        hc.invite_code = generate_invite_code().value();
        hc.share = share.get();
        server = std::make_unique<HostServer>(clock);
        auto started = server->start(hc);
        if (!started) { share->stop(); return false; }
        port = started.value();
        return true;
    }
    std::string invite() const { return server->snapshot()->invite_code; }
    void stop() {
        if (server) server->stop();
        if (share) share->stop();
    }
    ~Host() { stop(); }
};

struct Guest {
    SystemClock clock;
    std::unique_ptr<GuestClient> control;
    ShareGuest media;
    ParticipantId self{};

    bool join(Host& host, const std::string& name) {
        control = std::make_unique<GuestClient>(clock);
        GuestConfig gc;
        gc.address = IpAddress::loopback(IpFamily::Ipv4);
        gc.control_port = host.port;
        gc.guest_name = name;
        auto decoded = decode_invite_code(host.invite());
        if (!decoded) return false;
        gc.invite_code.bytes = decoded.value();
        gc.invite_code.text = host.invite();
        gc.tls_required = true;
        if (!control->start(gc)) return false;
        if (!control->wait_for([](const GuestSnapshot& s) { return s.phase == GuestPhase::Connected && s.media_offered; }, 10s))
            return false;
        auto snap = control->snapshot();
        self = snap->self;
        auto offer = control->media_offer();
        if (!offer) return false;
        ShareGuestConfig mc;
        mc.host = IpAddress::loopback(IpFamily::Ipv4);
        mc.offer = *offer;
        mc.session = SessionId{snap->session_id};
        mc.self = self;
        if (!media.start(mc)) return false;
        return wait_media([](const MediaGuestSnapshot& s) { return s.phase == MediaPhase::Streaming && s.frames > 0; }, 15s);
    }
    template <class P> bool wait_media(P pred, std::chrono::milliseconds timeout) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            if (pred(media.snapshot())) return true;
            std::this_thread::sleep_for(20ms);
        }
        return pred(media.snapshot());
    }
    // Waits for a decoded frame (received over the network) matching pred.
    template <class P> bool wait_pixels(P pred, std::chrono::milliseconds timeout) {
        const auto until = Clock::now() + timeout;
        std::uint64_t after = 0;
        while (Clock::now() < until) {
            if (auto img = media.latest(after)) {
                after = img->sequence;
                if (pred(img->image)) return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
    void click(float x, float y) {
        media.send_input(ev(media::InputEventType::MouseMove, x, y));
        media.send_input(ev(media::InputEventType::MouseDown, x, y, 1));
        media.send_input(ev(media::InputEventType::MouseUp, x, y, 1));
    }
    void stop() {
        media.stop();
        if (control) control->stop();
    }
    ~Guest() { stop(); }
};

std::optional<cdp::Value> dom(Host& host, ParticipantId who = {}) {
    auto raw = host.share->evaluate_for_diagnostics(who, "window.devlinkFixture ? window.devlinkFixture() : null");
    if (!raw) return std::nullopt;
    auto outer = cdp::parse(raw.value());
    if (!outer || outer->kind != cdp::Value::Kind::String) return std::nullopt;
    return cdp::parse(outer->text);
}
double clicks(Host& host, ParticipantId who = {}) {
    auto d = dom(host, who);
    return d ? d->number_at("clicks").value_or(-1) : -1;
}
const SharePeerSnapshot* peer(const ShareSnapshot& s, ParticipantId id) {
    for (const auto& p : s.peers)
        if (p.id == id) return &p;
    return nullptr;
}

void mode_b_real_pipeline_two_guests_exclusive_control() {
    Host host;
    REQUIRE(host.start(ShareMode::Collaborative));
    auto s0 = host.share->snapshot();
    CHECK(s0.stage == ShareStage::Active);
    std::printf("METRIC share_start_to_first_real_frame_ms=%llu\n", static_cast<unsigned long long>(s0.first_frame_ms));
    Guest alice, bob;
    REQUIRE(alice.join(host, "Alice"));
    REQUIRE(bob.join(host, "Bob"));
    const auto run = g_service->run_id();
    // The run id, read FROM PIXELS decoded by each guest after the network path.
    CHECK(alice.wait_pixels([&](const media::DecodedImage& i) { return plpt_fixture::read_run_band(i) == run; }, 10s));
    CHECK(bob.wait_pixels([&](const media::DecodedImage& i) { return plpt_fixture::read_run_band(i) == run; }, 10s));
    auto am = alice.media.snapshot();
    CHECK(am.transport.rfind("TLSv1.3", 0) == 0);
    CHECK_EQ(am.frame_width, 1280U);
    std::printf("EVIDENCE guest_read_run_id=%s transport=%s\n", run.c_str(), am.transport.c_str());

    // Nobody controls yet: Alice's click is refused server-side.
    alice.click(kButtonX, kButtonY);
    std::this_thread::sleep_for(600ms);
    CHECK_EQ(clicks(host), 0.0);
    // Alice takes the free lease, clicks: DOM and BOTH guests' pixels change.
    REQUIRE(alice.media.request_control());
    REQUIRE(alice.wait_media([&](const MediaGuestSnapshot& s) { return s.state.controller == alice.self.value && s.state.you_may_input; }, 5s));
    CHECK(bob.wait_media([&](const MediaGuestSnapshot& s) { return s.state.controller == alice.self.value && !s.state.you_may_input; }, 5s));
    alice.click(kButtonX, kButtonY);
    CHECK(alice.wait_pixels([](const media::DecodedImage& i) { return plpt_fixture::square_colour(i) == 1; }, 5s));
    CHECK(bob.wait_pixels([](const media::DecodedImage& i) { return plpt_fixture::square_colour(i) == 1; }, 5s));
    CHECK_EQ(clicks(host), 1.0);
    // Bob is not the controller: refused by the host, whatever his UI does.
    bob.click(kButtonX, kButtonY);
    std::this_thread::sleep_for(600ms);
    CHECK_EQ(clicks(host), 1.0);
    {
        auto s = host.share->snapshot();
        const auto* b = peer(s, bob.self);
        REQUIRE(b);
        CHECK(b->inputs_rejected >= 1);
        CHECK(b->last_rejection == input_decision_text(InputDecision::NotController));
    }
    // Transfer: Bob queues, Alice releases, Bob controls; Alice is now refused.
    REQUIRE(bob.media.request_control());
    CHECK(bob.wait_media([](const MediaGuestSnapshot& s) { return s.state.you_waiting; }, 5s));
    REQUIRE(alice.media.release_control());
    REQUIRE(bob.wait_media([&](const MediaGuestSnapshot& s) { return s.state.controller == bob.self.value && s.state.you_may_input; }, 5s));
    alice.click(kButtonX, kButtonY);
    std::this_thread::sleep_for(600ms);
    CHECK_EQ(clicks(host), 1.0);
    bob.click(kButtonX, kButtonY);
    CHECK(alice.wait_pixels([](const media::DecodedImage& i) { return plpt_fixture::square_colour(i) == 2; }, 5s));
    CHECK_EQ(clicks(host), 2.0);
    // Unicode text by the controller reaches the host DOM.
    bob.click(kFieldX, kFieldY);
    media::InputEvent text = ev(media::InputEventType::TextInput);
    text.text = "Bob écrit ✓";
    CHECK(bob.media.send_input(text));
    std::this_thread::sleep_for(500ms);
    {
        auto d = dom(host);
        REQUIRE(d);
        CHECK(d->string_at("value") == std::optional<std::string_view>("Bob écrit ✓"));
    }
    // Host takes control back from everyone.
    CHECK(host.share->grant_control(ParticipantId{}));
    CHECK(bob.wait_media([](const MediaGuestSnapshot& s) { return s.state.controller == 0 && !s.state.you_may_input; }, 5s));
    bob.click(kButtonX, kButtonY);
    std::this_thread::sleep_for(500ms);
    CHECK_EQ(clicks(host), 2.0);

    // Throughput while the fixture animates, measured on the guest.
    std::this_thread::sleep_for(3s);
    am = alice.media.snapshot();
    std::printf("METRIC guest_fps=%.1f guest_kbps=%.0f decode_ms_avg=%.2f input_rtt_ms_avg=%.1f input_rtt_ms_last=%.1f\n",
        am.fps, am.kbps, am.decode_ms_avg, am.input_rtt_ms_avg, am.input_rtt_ms_last);
    CHECK(am.fps >= 5.0);
    CHECK(am.invalid_records == 0);
    CHECK(am.decode_failures == 0);

    // Revocation: Bob's media channel is cut, his inputs never reach the page.
    REQUIRE(host.server->revoke(bob.self));
    CHECK(bob.wait_media([](const MediaGuestSnapshot& s) { return s.phase == MediaPhase::Closed || s.phase == MediaPhase::Failed; }, 5s));
    CHECK(!bob.media.send_input(ev(media::InputEventType::MouseMove, 0.5f, 0.5f)));
    CHECK(host.share->grant_control(bob.self) == false); // revoked: not grantable

    // Host stops: guests are told, nothing remains.
    host.stop();
    CHECK(alice.wait_media([](const MediaGuestSnapshot& s) { return s.phase == MediaPhase::Closed; }, 5s));
    auto end = host.share->snapshot();
    CHECK(end.stage == ShareStage::Stopped);
    CHECK_EQ(end.contexts_created, end.contexts_destroyed);
    host.share->stop(); // idempotent
}

void mode_c_observe_only_refuses_input_server_side() {
    Host host;
    REQUIRE(host.start(ShareMode::ObserveOnly));
    Guest viewer;
    REQUIRE(viewer.join(host, "Observateur"));
    CHECK(viewer.wait_pixels([&](const media::DecodedImage& i) { return plpt_fixture::read_run_band(i) == g_service->run_id(); }, 10s));
    // A modified client sends perfectly valid input records anyway.
    CHECK(!viewer.media.snapshot().state.you_may_input);
    viewer.media.request_control();
    viewer.click(kButtonX, kButtonY);
    media::InputEvent text = ev(media::InputEventType::TextInput);
    text.text = "intrusion";
    viewer.media.send_input(text);
    std::this_thread::sleep_for(800ms);
    CHECK_EQ(clicks(host), 0.0);
    auto d = dom(host);
    REQUIRE(d);
    CHECK(d->string_at("value") == std::optional<std::string_view>(""));
    auto s = host.share->snapshot();
    const auto* p = peer(s, viewer.self);
    REQUIRE(p);
    CHECK(p->inputs_rejected >= 3);
    CHECK_EQ(p->inputs_accepted, 0U);
    CHECK(p->last_rejection == input_decision_text(InputDecision::ObserveOnlyMode));
    CHECK(s.controller.value == 0);
}

void mode_a_independent_contexts_per_guest() {
    Host host;
    REQUIRE(host.start(ShareMode::Independent));
    Guest a, b;
    REQUIRE(a.join(host, "Testeur A"));
    REQUIRE(b.join(host, "Testeur B"));
    REQUIRE(a.wait_media([](const MediaGuestSnapshot& s) { return s.state.context_ready && s.state.you_may_input; }, 15s));
    REQUIRE(b.wait_media([](const MediaGuestSnapshot& s) { return s.state.context_ready && s.state.you_may_input; }, 15s));
    CHECK(host.share->context_of(a.self) != host.share->context_of(b.self));
    CHECK(host.share->context_of(a.self).value != 0);
    a.click(kButtonX, kButtonY);
    CHECK(a.wait_pixels([](const media::DecodedImage& i) { return plpt_fixture::square_colour(i) == 1; }, 8s));
    CHECK(b.wait_pixels([](const media::DecodedImage& i) { return plpt_fixture::square_colour(i) == 0; }, 3s));
    CHECK_EQ(clicks(host, a.self), 1.0);
    CHECK_EQ(clicks(host, b.self), 0.0);
    REQUIRE(host.share->evaluate_for_diagnostics(a.self, "localStorage.setItem('devlink-owner','A');document.cookie='t=A';1"));
    auto db = dom(host, b.self);
    REQUIRE(db);
    const auto* owner = db->get("owner");
    CHECK(owner && owner->kind == cdp::Value::Kind::Null);
    CHECK(db->string_at("cookie") == std::optional<std::string_view>(""));
    // Revoking A destroys A's browser context (its cookies and storage).
    const auto created_before = host.share->snapshot().contexts_destroyed;
    REQUIRE(host.server->revoke(a.self));
    const auto until = Clock::now() + 10s;
    while (host.share->snapshot().contexts_destroyed == created_before && Clock::now() < until) std::this_thread::sleep_for(50ms);
    CHECK(host.share->snapshot().contexts_destroyed > created_before);
    CHECK(host.share->context_of(a.self).value == 0);
}

void media_channel_refuses_hostile_peers() {
    Host host;
    REQUIRE(host.start(ShareMode::Collaborative));
    Guest alice;
    REQUIRE(alice.join(host, "Alice"));
    const auto offer = alice.control->media_offer();
    REQUIRE(offer);
    const auto session = SessionId{alice.control->snapshot()->session_id};
    const auto failed_before = host.share->snapshot().handshakes_failed;
    auto attempt = [&](ShareGuestConfig cfg) {
        ShareGuest g;
        if (!g.start(cfg)) return MediaPhase::Failed;
        const auto until = Clock::now() + 8s;
        while (Clock::now() < until) {
            auto s = g.snapshot();
            if (s.phase == MediaPhase::Failed || s.phase == MediaPhase::Closed) return s.phase;
            if (s.phase == MediaPhase::Streaming && s.frames > 0) return s.phase;
            std::this_thread::sleep_for(20ms);
        }
        return g.snapshot().phase;
    };
    ShareGuestConfig base;
    base.host = IpAddress::loopback(IpFamily::Ipv4);
    base.offer = *offer;
    base.session = session;
    base.self = alice.self;
    // Wrong key (one hex digit flipped).
    auto wrong_key = base;
    wrong_key.offer.key[0] = wrong_key.offer.key[0] == 'a' ? 'b' : 'a';
    CHECK(attempt(wrong_key) != MediaPhase::Streaming);
    // Right key, usurped participant id.
    auto usurp = base;
    usurp.self = ParticipantId{alice.self.value + 1000};
    CHECK(attempt(usurp) != MediaPhase::Streaming);
    // Wrong session.
    auto other_session = base;
    other_session.session = SessionId{session.value + 1};
    CHECK(attempt(other_session) != MediaPhase::Streaming);
    // Garbage before TLS, then a TLS 1.2-only client (downgrade).
    {
        auto s = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
        REQUIRE(s);
        auto sock = std::move(s).value();
        REQUIRE(connect_with_timeout(sock, IpAddress::loopback(IpFamily::Ipv4), Port{offer->port}, 2000ms));
        const std::uint8_t junk[] = {0, 0, 0, 200, 'G', 'E', 'T', ' '};
        (void)sock.send(junk);
        std::this_thread::sleep_for(300ms);
        sock.close();
    }
    {
        auto s = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
        REQUIRE(s);
        auto sock = std::move(s).value();
        REQUIRE(connect_with_timeout(sock, IpAddress::loopback(IpFamily::Ipv4), Port{offer->port}, 2000ms));
        (void)sock.set_nonblocking(false);
        const auto hello = encode_hello(MediaHello{session.value, alice.self.value});
        REQUIRE(sock.send(hello));
        TlsPolicy tls12;
        tls12.min_version = kTls12Version;
        tls12.max_version = kTls12Version;
        auto factory = make_tls13_factory(tls12);
        REQUIRE(factory);
        auto downgraded = factory.value()->wrap_client(static_cast<TransportDescriptor>(sock.release()),
            TransportCredentials{offer->identity, offer->key}, 3000);
        CHECK(!downgraded); // the host only speaks TLS 1.3: no downgrade, no clear fallback
    }
    std::this_thread::sleep_for(300ms);
    CHECK(host.share->snapshot().handshakes_failed >= failed_before + 4);
    // Alice's legitimate stream survived every attempt.
    CHECK(alice.wait_pixels([](const media::DecodedImage&) { return true; }, 3s) ||
        alice.media.snapshot().phase == MediaPhase::Streaming);
    // Replay inside the channel: a record re-using an already consumed sequence.
    REQUIRE(alice.media.request_control());
    REQUIRE(alice.wait_media([](const MediaGuestSnapshot& s) { return s.state.you_may_input; }, 5s));
    InputRecord replay;
    replay.input_seq = 1;
    replay.event = ev(media::InputEventType::MouseDown, kButtonX, kButtonY, 1);
    alice.click(kButtonX, kButtonY); // consumes sequences 1..3 (move, down, up)
    std::this_thread::sleep_for(400ms);
    const double before = clicks(host);
    REQUIRE(alice.media.send_raw_for_test(encode_input(replay).value()));
    replay.event = ev(media::InputEventType::MouseUp, kButtonX, kButtonY, 1);
    replay.input_seq = 2;
    REQUIRE(alice.media.send_raw_for_test(encode_input(replay).value()));
    std::this_thread::sleep_for(500ms);
    CHECK_EQ(clicks(host), before);
    auto s = host.share->snapshot();
    const auto* p = peer(s, alice.self);
    REQUIRE(p);
    CHECK(p->last_rejection == input_decision_text(InputDecision::Replayed));
    // Oversized record length: the host drops the channel instead of allocating.
    std::vector<std::uint8_t> huge = {0xFF, 0xFF, 0xFF, 0xF0, static_cast<std::uint8_t>(RecordKind::Input)};
    REQUIRE(alice.media.send_raw_for_test(huge));
    CHECK(alice.wait_media([](const MediaGuestSnapshot& m) { return m.phase == MediaPhase::Closed || m.phase == MediaPhase::Failed; }, 5s));
    // Leaving invalidates the grant: the old key no longer opens anything.
    alice.control->leave();
    std::this_thread::sleep_for(500ms);
    CHECK(attempt(base) != MediaPhase::Streaming);
}

void share_refuses_unsafe_starts_and_cycles_cleanly() {
    // No real engine: refused, never a silent synthetic demo.
    {
        ShareHost fake(std::make_unique<DemoBrowserBackend>());
        ShareHostConfig c;
        c.allowed_origins = {g_service->origin()};
        c.entry_url = g_service->origin() + "/fixture";
        auto r = fake.start(c);
        CHECK(!r);
        if (!r) CHECK_EQ(r.error().code, ErrorCode::Unsupported);
        CHECK(fake.snapshot().stage == ShareStage::Failed);
        CHECK_EQ(fake.snapshot().media_port, 0);
    }
    // Entry URL outside the explicit selection / no selection.
    {
        ShareHost h(std::make_unique<browser::WebView2Backend>());
        ShareHostConfig c;
        c.allowed_origins = {g_service->origin()};
        c.entry_url = "http://127.0.0.1:5432/";
        CHECK(!h.start(c));
        c.allowed_origins.clear();
        c.entry_url = g_service->origin() + "/fixture";
        CHECK(!h.start(c));
    }
    // Navigation to an allowed origin that redirects out: rollback, no listener.
    {
        ShareHost h(std::make_unique<browser::WebView2Backend>());
        ShareHostConfig c;
        c.allowed_origins = {g_service->origin()};
        c.entry_url = g_service->origin() + "/redirect-out";
        auto r = h.start(c);
        CHECK(!r);
        auto s = h.snapshot();
        CHECK(s.stage == ShareStage::Failed);
        CHECK_EQ(s.media_port, 0);
        CHECK_EQ(s.contexts_created, s.contexts_destroyed);
    }
    // Repeated start/stop cycles.
    for (int i = 0; i < 3; ++i) {
        Host host;
        REQUIRE(host.start(ShareMode::Collaborative));
        Guest g;
        REQUIRE(g.join(host, "Cycle"));
        host.stop();
        auto s = host.share->snapshot();
        CHECK(s.stage == ShareStage::Stopped);
        CHECK_EQ(s.contexts_created, s.contexts_destroyed);
    }
}

void five_guests_collaborative_load() {
    Host host;
    REQUIRE(host.start(ShareMode::Collaborative, 5));
    std::vector<std::unique_ptr<Guest>> guests;
    for (int i = 0; i < 5; ++i) {
        guests.push_back(std::make_unique<Guest>());
        REQUIRE(guests.back()->join(host, "Invité " + std::to_string(i + 1)));
    }
    std::this_thread::sleep_for(4s);
    double min_fps = 1e9, total_kbps = 0;
    for (auto& g : guests) {
        auto s = g->media.snapshot();
        min_fps = std::min(min_fps, s.fps);
        total_kbps += s.kbps;
        CHECK(s.invalid_records == 0);
        CHECK(g->wait_pixels([&](const media::DecodedImage& i) { return plpt_fixture::read_run_band(i) == g_service->run_id(); }, 5s));
    }
    std::printf("METRIC five_guests_min_fps=%.1f total_kbps=%.0f\n", min_fps, total_kbps);
    CHECK(min_fps >= 5.0);
    // A sixth guest is refused by the control plane (capacity), no media offered.
    Guest extra;
    CHECK(!extra.join(host, "Invité 6"));
}
#endif
} // namespace

int main() {
#if defined(_WIN32)
    if (!browser::WebView2Backend::is_available()) {
        std::printf("SKIP share: runtime WebView2 absent\n");
        return 77;
    }
    if (!tls13_runtime_available()) {
        std::printf("SKIP share: runtime TLS 1.3 absent\n");
        return 77;
    }
    plpt_fixture::FixtureService service;
    if (!service.start(plpt_fixture::random_run_id())) {
        std::printf("FAIL devlink-demo-service introuvable\n");
        return 1;
    }
    g_service = &service;
    CASE(mode_b_real_pipeline_two_guests_exclusive_control);
    CASE(mode_c_observe_only_refuses_input_server_side);
    CASE(mode_a_independent_contexts_per_guest);
    CASE(media_channel_refuses_hostile_peers);
    CASE(share_refuses_unsafe_starts_and_cycles_cleanly);
    CASE(five_guests_collaborative_load);
    service.stop();
    return plpt_test::finish("share");
#else
    std::printf("SKIP share: Browser Worker Windows-only (ADR-018)\n");
    return 77;
#endif
}
