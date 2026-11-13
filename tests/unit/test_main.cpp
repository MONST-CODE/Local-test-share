#include <array>
#include <atomic>
#include <limits>
#include <random>
#include <type_traits>
#include "tests/test_harness.hpp"
#include "engine/browser/browser.hpp"
#include "engine/core/random.hpp"
#include "engine/core/text.hpp"
#include "engine/diagnostics/audit.hpp"
#include "engine/discovery/monitor.hpp"
#include "engine/messaging/messaging.hpp"
#include "engine/protocol/envelope.hpp"
#include "engine/resource_manager/resource.hpp"
#include "engine/transport/transport.hpp"

using namespace plpt;
using namespace plpt::model;
static_assert(!std::is_convertible_v<ParticipantId, SessionId>);
static_assert(!std::is_convertible_v<std::uint64_t, ParticipantId>);

namespace {
ProcessRecord process(std::uint32_t pid, std::uint32_t ppid, std::string name, std::uint64_t created = 1) {
    return {ProcessId{pid}, ProcessId{ppid}, std::move(name), std::nullopt, created};
}
SocketRecord listener(std::uint16_t port, std::optional<ProcessId> pid = ProcessId{3}, TransportProtocol proto = TransportProtocol::Tcp) {
    return {proto, {IpFamily::Ipv4, "127.0.0.1", Port{port}}, {},
        proto == TransportProtocol::Tcp ? SocketState::Listening : SocketState::Bound, pid};
}
void ready(Session& session) {
    REQUIRE(session.transition_to(SessionState::Discovering));
    REQUIRE(session.transition_to(SessionState::AwaitingGuests));
}
void result_invariants() {
    auto move = Result<std::unique_ptr<int>>::success(std::make_unique<int>(7));
    CHECK_EQ(*std::move(move).value(), 7);
    auto failure = Result<int>::failure({ErrorCode::IoError, "test", "unit"});
    CHECK(!failure); CHECK_EQ(failure.error().code, ErrorCode::IoError);
    bool caught = false;
    try { (void)failure.value(); } catch (const std::bad_optional_access&) { caught = true; }
    CHECK(caught);
    caught = false;
    try { (void)Result<void>::failure({}); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught);
}
void exact_classifier_and_ancestry() {
    const auto tree = ProcessTree::build({process(1, 0, "WindowsTerminal.exe", 10), process(2, 1, "powershell.exe", 20),
        process(3, 2, "npm.cmd", 30), process(4, 3, "node.exe", 40)});
    CHECK_EQ(tree.size(), 4U);
    CHECK_EQ(tree.ancestors(ProcessId{4}).size(), 3U);
    const auto inferred = classify_origin(tree, ProcessId{4});
    CHECK_EQ(inferred.kind, OriginKind::Terminal);
    CHECK_EQ(inferred.confidence, Confidence::High);
    CHECK_EQ(inferred.alternatives.size(), 3U); // runner ne masque pas le terminal.
    const auto false_positive = ProcessTree::build({process(1, 0, "not-bash-server"), process(2, 1, "node")});
    CHECK_EQ(classify_origin(false_positive, ProcessId{2}).kind, OriginKind::Unknown);
}
void cycles_missing_and_reused_pid() {
    const auto cycle = ProcessTree::build({process(1, 2, "a"), process(2, 1, "b")});
    CHECK_EQ(cycle.ancestors(ProcessId{1}).size(), 1U);
    const auto self = ProcessTree::build({process(1, 1, "a")});
    CHECK(self.ancestors(ProcessId{1}).empty());
    const auto missing = ProcessTree::build({process(2, 99, "node")});
    REQUIRE(missing.find(ProcessId{2})); CHECK(missing.find(ProcessId{2})->parent_missing);
    const auto young = ProcessTree::build({process(1, 0, "bash", 300), process(2, 1, "node", 200)});
    CHECK(young.ancestors(ProcessId{2}).empty()); // parent né après l'enfant = PID recyclé.
    const std::map<ProcessId, std::uint64_t> previous{{ProcessId{2}, 100}};
    const auto reuse = ProcessTree::build({process(2, 0, "node", 200)}, &previous);
    CHECK(reuse.find(ProcessId{2})->pid_reused);
    CHECK(!ProcessTree::build({process(2, 0, "node", 0)}, &previous).find(ProcessId{2})->pid_reused);
}
void registry_grouping_and_no_auto_share() {
    ServiceRegistry registry;
    Observation<SocketRecord> snapshot{{listener(3000), listener(3001), listener(3002, ProcessId{3}, TransportProtocol::Udp)}, true, {}};
    auto outbound = listener(4000); outbound.state = SocketState::Established;
    auto fin = listener(4001); fin.state = SocketState::Connected;
    snapshot.records.push_back(outbound); snapshot.records.push_back(fin);
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 12)}, true, {}}, 100));
    CHECK_EQ(registry.live_count(), 1U);
    auto id = registry.services().begin()->first;
    REQUIRE(registry.find(id)); CHECK_EQ(registry.find(id)->endpoints.size(), 3U);
    CHECK_EQ(registry.find(id)->share_state, ShareState::Detected);
    CHECK(!registry.select(id, snapshot.records[2].local)); // UDP != point d'entrée Web.
    REQUIRE(registry.select(id, snapshot.records[0].local));
    CHECK_EQ(registry.find(id)->share_state, ShareState::Selected);
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 12)}, true, {}}, 100)); // timestamps égaux permis.
    CHECK_EQ(registry.find(id)->share_state, ShareState::Selected);
    snapshot.records.erase(snapshot.records.begin()); // endpoint sélectionné disparu, autre port présent.
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 12)}, true, {}}, 101));
    CHECK_EQ(registry.find(id)->share_state, ShareState::Detected);
    CHECK(!registry.find(id)->selected_endpoint);
    REQUIRE(registry.protect(id)); CHECK(!registry.select(id, snapshot.records[0].local));
}
void registry_churn_and_incomplete() {
    ServiceRegistry registry;
    Observation<SocketRecord> snapshot{{listener(3000)}, true, {}};
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 12)}, true, {}}, 0));
    const auto old = registry.services().begin()->first;
    REQUIRE(registry.select(old, snapshot.records[0].local));
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 13)}, true, {}}, 0)); // même PID, autre création.
    CHECK_EQ(registry.find(old)->share_state, ShareState::Gone);
    CHECK_EQ(registry.live_count(), 1U);
    const auto current = registry.services().rbegin()->first;
    CHECK(current != old); CHECK_EQ(registry.find(current)->share_state, ShareState::Detected);
    Observation<SocketRecord> partial; partial.complete = false;
    REQUIRE(registry.ingest(partial, {}, 0));
    CHECK_EQ(registry.find(current)->share_state, ShareState::Unavailable);
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 13)}, true, {}}, 1));
    CHECK_EQ(registry.find(current)->share_state, ShareState::Detected);
    REQUIRE(registry.ingest({}, {}, 1)); CHECK_EQ(registry.find(current)->share_state, ShareState::Gone);
    REQUIRE(registry.ingest(snapshot, {{process(3, 0, "node", 13)}, true, {}}, 2));
    CHECK(!registry.find(current)); // identifiant ancien jamais réactivé après disparition.
    CHECK_EQ(registry.services().size(), 1U);
}
void partial_process_inventory_invalidates_selection() {
    ServiceRegistry registry;
    Observation<SocketRecord> sockets{{listener(3000)}, true, {}};
    Observation<ProcessRecord> processes{{process(3, 0, "node", 12)}, true, {}};
    REQUIRE(registry.ingest(sockets, processes, 0));
    const auto id = registry.services().begin()->first;
    REQUIRE(registry.select(id, sockets.records[0].local));
    processes.complete = false; // même PID stable, table sockets complète : processus partiels.
    REQUIRE(registry.ingest(sockets, processes, 1));
    CHECK_EQ(registry.find(id)->share_state, ShareState::Unavailable);
    CHECK(!registry.find(id)->selected_endpoint);
    CHECK(!registry.select(id, sockets.records[0].local));
    REQUIRE(registry.ingest({}, processes, 2)); // absence non démontrée.
    CHECK_EQ(registry.find(id)->share_state, ShareState::Unavailable);
    processes.complete = true;
    REQUIRE(registry.ingest(sockets, processes, 3));
    CHECK_EQ(registry.find(id)->share_state, ShareState::Detected);
    CHECK(!registry.find(id)->selected_endpoint); // jamais restaurée silencieusement.
}
void registry_capacity_failure_invalidates_selection() {
    ServiceRegistry registry;
    Observation<SocketRecord> sockets{{listener(3000)}, true, {}};
    REQUIRE(registry.ingest(sockets, {{process(3, 0, "node", 12)}, true, {}}, 0));
    const auto id = registry.services().begin()->first;
    REQUIRE(registry.select(id, sockets.records[0].local));
    sockets.records.clear();
    for (unsigned n = 1; n <= 4097; ++n)
        sockets.records.push_back(listener(static_cast<std::uint16_t>(n), std::nullopt));
    auto result = registry.ingest(sockets, {}, 1);
    REQUIRE(!result);
    CHECK_EQ(result.error().code, ErrorCode::CapacityExceeded);
    CHECK_EQ(registry.find(id)->share_state, ShareState::Unavailable);
    CHECK(!registry.find(id)->selected_endpoint);
    CHECK(!registry.select(id, sockets.records[0].local));
    CHECK_EQ(registry.services().size(), 1U); // pas de publication partielle des 4097 groupes.
}
void partial_registry_history_is_bounded() {
    ServiceRegistry registry;
    Observation<SocketRecord> partial;
    partial.complete = false;
    for (unsigned n = 1; n <= 4096; ++n) partial.records.push_back(listener(static_cast<std::uint16_t>(n), std::nullopt));
    REQUIRE(registry.ingest(partial, {}, 1));
    CHECK_EQ(registry.services().size(), 4096U);
    for (auto& endpoint : partial.records) endpoint.local.port.value = static_cast<std::uint16_t>(endpoint.local.port.value + 4096);
    REQUIRE(registry.ingest(partial, {}, 2));
    CHECK_EQ(registry.services().size(), 4096U);
    CHECK_EQ(registry.live_count(), 0U);
}
void unknown_owner_cannot_retain_selection() {
    ServiceRegistry registry;
    Observation<SocketRecord> snapshot{{listener(3000, std::nullopt)}, true, {}};
    REQUIRE(registry.ingest(snapshot, {}, 1));
    const auto id = registry.services().begin()->first;
    REQUIRE(registry.select(id, snapshot.records[0].local));
    REQUIRE(registry.ingest(snapshot, {}, 2));
    CHECK_EQ(registry.find(id)->share_state, ShareState::Detected);
}
void state_expiry_and_cleanup() {
    ManualClock clock(100);
    Session session(SessionId{1}, {5, 1000}, clock);
    CHECK(!session.transition_to(SessionState::Active));
    CHECK(!session.admit_verified_guest(Role::Observer, ContextId{1}));
    ready(session);
    CHECK(!session.admit_verified_guest(Role::Host, ContextId{1}));
    CHECK(!session.admit_verified_guest(Role::Observer, ContextId{}));
    auto guest = session.admit_verified_guest(Role::Observer, ContextId{1}); REQUIRE(guest);
    CHECK_EQ(session.state(), SessionState::Active);
    CHECK(session.can(guest.value(), Capability::ViewStream));
    clock.advance_ms(1000); // expiration exacte, pas une milliseconde de grâce implicite.
    CHECK(session.is_expired()); CHECK(!session.can(guest.value(), Capability::SendChat));
    CHECK(!session.admit_verified_guest(Role::Observer, ContextId{2}));
    REQUIRE(session.transition_to(SessionState::Closing));
    REQUIRE(session.transition_to(SessionState::Cleanup));
    CHECK(!session.transition_to(SessionState::Closed));
    CHECK(!session.complete_cleanup({false, 0, {}})); CHECK_EQ(session.state(), SessionState::Cleanup);
    REQUIRE(session.complete_cleanup({true, 0, {}}));
    CHECK_EQ(session.state(), SessionState::Closed);
    CHECK(!session.admit_verified_guest(Role::Observer, ContextId{3}));
}
void closing_revokes_without_waiting_for_expiry() {
    ManualClock clock;
    Session session(SessionId{1}, {}, clock); ready(session);
    auto p = session.admit_verified_guest(Role::IndependentTester, ContextId{1}); REQUIRE(p);
    REQUIRE(session.transition_to(SessionState::Closing));
    CHECK(!session.can(p.value(), Capability::SendInput));
    CHECK(!session.can(p.value(), Capability::SendChat));
    CHECK(!session.set_participant_state(p.value(), ParticipantState::Connected));
}
void roles_contexts_control_and_revocation() {
    ManualClock clock;
    Session session(SessionId{1}, {}, clock); ready(session);
    auto obs = session.admit_verified_guest(Role::Observer, ContextId{1}); REQUIRE(obs);
    auto first = session.admit_verified_guest(Role::Collaborator, ContextId{1}); REQUIRE(first);
    auto second = session.admit_verified_guest(Role::Collaborator, ContextId{1}); REQUIRE(second);
    auto independent = session.admit_verified_guest(Role::IndependentTester, ContextId{2}); REQUIRE(independent);
    CHECK(!session.admit_verified_guest(Role::IndependentTester, ContextId{2}));
    CHECK(!session.admit_verified_guest(Role::Observer, ContextId{2}));
    CHECK(!session.can(obs.value(), Capability::SendInput)); CHECK(!session.grant_control(obs.value()));
    CHECK(!session.can(first.value(), Capability::SendInput));
    REQUIRE(session.grant_control(first.value())); CHECK(session.can_input(first.value(), ContextId{1}));
    CHECK(!session.can_input(first.value(), ContextId{2}));
    REQUIRE(session.grant_control(second.value())); CHECK(!session.can(first.value(), Capability::SendInput));
    CHECK(session.can(second.value(), Capability::SendInput));
    REQUIRE(session.set_participant_state(second.value(), ParticipantState::Reconnecting));
    CHECK(!session.can(second.value(), Capability::ViewStream));
    REQUIRE(session.set_participant_state(second.value(), ParticipantState::Connected));
    CHECK(!session.can(second.value(), Capability::SendInput)); // lock non repris implicitement.
    CHECK(session.can_input(independent.value(), ContextId{2}));
    REQUIRE(session.revoke_participant(independent.value())); REQUIRE(session.revoke_participant(independent.value()));
    CHECK(!session.can(independent.value(), Capability::SendChat));
    CHECK(!session.set_participant_state(independent.value(), ParticipantState::Connected));
    CHECK(!role_has_capability(Role::Host, static_cast<Capability>(255)));
    CHECK(!role_has_capability(static_cast<Role>(255), Capability::ViewStream));
}
void five_model_guests_and_capacity() {
    ManualClock clock;
    Session session(SessionId{1}, {}, clock); ready(session);
    for (std::uint64_t i = 1; i <= 5; ++i) CHECK(session.admit_verified_guest(Role::IndependentTester, ContextId{i}));
    auto sixth = session.admit_verified_guest(Role::IndependentTester, ContextId{6});
    REQUIRE(!sixth); CHECK_EQ(sixth.error().code, ErrorCode::CapacityExceeded);
    CHECK_EQ(session.active_count(), 5U); // modèles, PAS cinq navigateurs ou cinq PCs.
}
void messages_dm_privacy_and_read_receipts() {
    ManualClock clock;
    Session session(SessionId{1}, {}, clock); ready(session);
    auto a = session.admit_verified_guest(Role::Observer, ContextId{1}); REQUIRE(a);
    auto b = session.admit_verified_guest(Role::Observer, ContextId{1}); REQUIRE(b);
    auto c = session.admit_verified_guest(Role::Observer, ContextId{1}); REQUIRE(c);
    Messaging messages(session);
    auto dm = messages.post_dm(a.value(), b.value(), "privé"); REQUIRE(dm);
    REQUIRE(messages.reply(dm.value(), b.value(), "réponse privée"));
    CHECK(!messages.reply(dm.value(), c.value(), "intrusion"));
    REQUIRE(messages.general_channel(a.value())); CHECK(messages.general_channel(a.value()).value().empty());
    REQUIRE(messages.dm_channel(b.value(), a.value())); CHECK_EQ(messages.dm_channel(b.value(), a.value()).value().size(), 2U);
    CHECK(messages.dm_channel(c.value(), a.value()).value().empty());
    CHECK(!messages.mark_read(c.value(), dm.value())); REQUIRE(messages.mark_read(b.value(), dm.value()));
    CHECK_EQ(messages.dm_channel(a.value(), b.value()).value()[0].readers.size(), 1U);
    CHECK(!messages.post_dm(a.value(), ParticipantId{999}, "unknown"));
    REQUIRE(session.revoke_participant(b.value()));
    CHECK(!messages.post_dm(a.value(), b.value(), "revoked")); CHECK(!messages.dm_channel(b.value(), a.value()));
    messages.clear_all(); CHECK(messages.dm_channel(a.value(), b.value()).value().empty());
}
void messages_quotas_and_feedback_redaction() {
    ManualClock clock;
    Session session(SessionId{1}, {}, clock); ready(session);
    auto a = session.admit_verified_guest(Role::IndependentTester, ContextId{42}); REQUIRE(a);
    Messaging messages(session);
    CHECK(!messages.post_general(a.value(), ""));
    CHECK(!messages.post_general(a.value(), std::string(4097, 'x')));
    CHECK(!messages.post_general(a.value(), "\x1b[31mANSI"));
    for (int i = 0; i < 10; ++i) REQUIRE(messages.post_general(a.value(), "message"));
    auto rate = messages.post_general(a.value(), "flood"); REQUIRE(!rate); CHECK_EQ(rate.error().code, ErrorCode::RateLimited);
    clock.advance_ms(1000);
    IssueReport report; report.description = "bug"; report.browser_context = ContextId{999};
    report.page_route = "https://user:secret@local/account/token-in-path?secret=x#secret";
    REQUIRE(messages.report_issue(a.value(), report));
    CHECK(messages.host_reports()[0].page_route.empty());
    CHECK_EQ(messages.host_reports()[0].browser_context, ContextId{42});
    report.has_screenshot = true; CHECK(!messages.report_issue(a.value(), report));
    messages.clear_all();
    for (int i = 0; i < 256; ++i) { clock.advance_ms(1000); REQUIRE(messages.post_general(a.value(), std::string(4096, 'x'))); }
    auto full = messages.post_general(a.value(), "over budget"); REQUIRE(!full); CHECK_EQ(full.error().code, ErrorCode::CapacityExceeded);
}
void text_validation() {
    CHECK(valid_text("Bonjour à Yaoundé 👍\n\t"));
    for (const auto& s : {std::string("\0", 1), std::string("\xC0\xAF"), std::string("\xED\xA0\x80"),
        std::string("\xF4\x90\x80\x80"), std::string("\xE2\x82"), std::string("\xC2\x80")}) CHECK(!valid_text(s));
}
Envelope chat() {
    Envelope e; e.type = MessageType::Chat; e.session = SessionId{0x0102030405060708}; e.participant = ParticipantId{2};
    e.stream_id = 4; e.sequence = 1; e.payload = {'o','k'}; return e;
}
void protocol_golden_and_bounds() {
    auto e = chat(); auto encoded = encode_envelope(e); REQUIRE(encoded);
    CHECK_EQ(encoded.value().size(), 58U);
    CHECK_EQ(encoded.value()[0], 'P'); CHECK_EQ(encoded.value()[3], 'T');
    CHECK_EQ(encoded.value()[12], 1); CHECK_EQ(encoded.value()[19], 8); CHECK_EQ(encoded.value()[51], 2);
    auto decoded = decode_envelope(encoded.value()); REQUIRE(decoded);
    CHECK_EQ(decoded.value().session, e.session); CHECK_EQ(decoded.value().payload, e.payload);
    for (std::size_t n = 0; n < encoded.value().size(); ++n) CHECK(!decode_envelope(std::span(encoded.value()).first(n)));
    auto extra = encoded.value(); extra.push_back(0); CHECK(!decode_envelope(extra));
    for (auto offset : {std::size_t{0}, std::size_t{10}, std::size_t{48}, std::size_t{55}}) {
        auto corrupt = encoded.value(); corrupt[offset] = 0xFF; CHECK(!decode_envelope(corrupt));
    }
    CHECK(!negotiate_version(2, 0)); CHECK(!negotiate_version(1, 1));
    e.payload.resize(8193); CHECK(!encode_envelope(e));
    e = chat(); e.type = static_cast<MessageType>(999); CHECK(!encode_envelope(e));
    e = chat(); e.flags = 1; CHECK(!encode_envelope(e));
    e = chat(); e.participant = {}; CHECK(!encode_envelope(e));
    e = chat(); e.sequence = 0; CHECK(!encode_envelope(e));
}
void protocol_deterministic_mutations() {
    std::mt19937 rng(20261113); // tests uniquement, jamais un secret.
    const auto seed = encode_envelope(chat()); REQUIRE(seed);
    for (int i = 0; i < 10000; ++i) {
        auto bytes = seed.value();
        bytes[rng() % bytes.size()] = static_cast<std::uint8_t>(rng());
        auto decoded = decode_envelope(bytes);
        if (decoded) {
            auto roundtrip = encode_envelope(decoded.value()); REQUIRE(roundtrip); CHECK_EQ(roundtrip.value(), bytes);
        }
    }
}
void estimates_are_bounded() {
    ResourceSample sample{20, 4000, 16000, 10, 0};
    CHECK_EQ(decide_capacity(sample, 5).additional_guests, 5U);
    sample.active_profiles = 5; CHECK(!decide_capacity(sample, 5).accept_new_guest);
    sample.active_profiles = 0; sample.cpu_percent = std::numeric_limits<double>::quiet_NaN(); CHECK(!decide_capacity(sample, 5).accept_new_guest);
    sample.cpu_percent = 90; CHECK_EQ(decide_capacity(sample, 5).quality, QualityLevel::Low480p);
    sample.ram_total_mb = std::numeric_limits<double>::infinity(); CHECK(!decide_capacity(sample, 5).accept_new_guest);
}
void audit_is_opt_in_typed_and_bounded() {
    ManualClock clock(500);
    AuditLog off(clock); off.record(AuditEvent::GuestConnected); CHECK(off.entries().empty());
    AuditLog on(clock, true);
    for (int i = 0; i < 1000; ++i) on.record(AuditEvent::ParticipantRevoked);
    CHECK_EQ(on.entries().size(), 256U); CHECK_EQ(on.entries().back().timestamp_ms, 500U);
    on.clear(); CHECK(on.entries().empty());
}
void unavailable_backends_fail_closed() {
    NullBrowserBackend browser;
    CHECK(!browser.create_context(ParticipantId{1}, {})); CHECK(!browser.capture_frame(ContextId{1}));
    UnavailableTemporaryStorage storage;
    CHECK(!storage.create_session_dir(SessionId{1}, ParticipantId{1}));
    CHECK(!storage.destroy_session_dir(SessionId{1}, ParticipantId{1}));
    CHECK(!storage.sweep_verified_orphans());
}
class FakeProcesses final : public IProcessEnumerator {
public:
    bool reuse = false, partial_first = false; unsigned call = 0;
    Result<Observation<ProcessRecord>> enumerate() override {
        ++call;
        Observation<ProcessRecord> out{{process(3, 0, "test", reuse && call > 1 ? 12U : 11U)}, true, {}};
        if (partial_first && call == 1) {
            out.complete = false;
            out.warnings.push_back({ErrorCode::IoError, "fixture", "process-before"});
        }
        return Result<Observation<ProcessRecord>>::success(std::move(out));
    }
};
class FakeSockets final : public ISocketTableProvider {
public:
    Result<Observation<SocketRecord>> snapshot() override {
        return Result<Observation<SocketRecord>>::success({{listener(32123)}, true, {}});
    }
};
void observation_brackets_process_identity() {
    FakeProcesses processes; FakeSockets sockets;
    auto valid = observe(processes, sockets); REQUIRE(valid); CHECK(valid.value().sockets.records[0].owner_pid);
    processes.call = 0; processes.reuse = true;
    auto recycled = observe(processes, sockets); REQUIRE(recycled); CHECK(!recycled.value().sockets.records[0].owner_pid);
}
void observation_keeps_first_snapshot_warning() {
    FakeProcesses processes; processes.partial_first = true;
    FakeSockets sockets;
    auto partial = observe(processes, sockets); REQUIRE(partial);
    CHECK(!partial.value().processes.complete);
    REQUIRE(partial.value().processes.warnings.size() == 1);
    CHECK_EQ(partial.value().processes.warnings[0].context, "process-before");
}
enum class ProcessMode { Complete, Partial, Failure };
class ControlledProcesses final : public IProcessEnumerator {
public:
    explicit ControlledProcesses(std::shared_ptr<std::atomic<ProcessMode>> mode) : mode_(std::move(mode)) {}
    Result<Observation<ProcessRecord>> enumerate() override {
        const auto mode = mode_->load();
        if (mode == ProcessMode::Failure)
            return Result<Observation<ProcessRecord>>::failure({ErrorCode::IoError, "fixture unavailable", "processes"});
        Observation<ProcessRecord> out{{process(3, 0, "test", 11)}, mode == ProcessMode::Complete, {}};
        if (mode == ProcessMode::Partial)
            out.warnings.push_back({ErrorCode::IoError, "fixture interrupted", "processes"});
        return Result<Observation<ProcessRecord>>::success(std::move(out));
    }
private:
    std::shared_ptr<std::atomic<ProcessMode>> mode_;
};
template <typename Predicate> bool wait_view(Monitor& monitor, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto view = monitor.view();
        if (predicate(*view)) return true;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0 || !monitor.wait_for_revision(view->revision + 1, remaining)) return false;
    }
}
void worker_invalidates_selection_on_partial_or_error() {
    const auto mode = std::make_shared<std::atomic<ProcessMode>>(ProcessMode::Complete);
    Monitor monitor(std::make_unique<ControlledProcesses>(mode), std::make_unique<FakeSockets>());
    REQUIRE(monitor.wait_for_revision(1, std::chrono::seconds(5)));
    const auto first = monitor.view(); REQUIRE(first->services.size() == 1);
    const auto id = first->services[0].service_id;
    const auto endpoint = first->services[0].endpoints[0].local;
    for (const auto incomplete : {ProcessMode::Partial, ProcessMode::Failure}) {
        REQUIRE(monitor.request_selection(id, endpoint));
        REQUIRE(wait_view(monitor, [](const auto& view) {
            return view.services.size() == 1 && view.services[0].share_state == ShareState::Selected;
        }));
        const auto selected = monitor.view();
        mode->store(incomplete); REQUIRE(monitor.request_refresh());
        REQUIRE(wait_view(monitor, [](const auto& view) {
            return !view.complete && view.services.size() == 1 && view.services[0].share_state == ShareState::Unavailable;
        }));
        const auto unavailable = monitor.view();
        CHECK(!unavailable->services[0].selected_endpoint);
        CHECK(!unavailable->warnings.empty());
        CHECK_EQ(selected->services[0].share_state, ShareState::Selected); // immutable.
        mode->store(ProcessMode::Complete); REQUIRE(monitor.request_refresh());
        REQUIRE(wait_view(monitor, [](const auto& view) {
            return view.complete && view.services.size() == 1 && view.services[0].share_state == ShareState::Detected;
        }));
        CHECK(!monitor.view()->services[0].selected_endpoint);
        CHECK_EQ(monitor.view()->services[0].service_id, id);
    }
}
struct SnapshotGate {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false, released = false;
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        ready.notify_all();
    }
};
class GatedSockets final : public ISocketTableProvider {
public:
    explicit GatedSockets(std::shared_ptr<SnapshotGate> gate) : gate_(std::move(gate)) {}
    Result<Observation<SocketRecord>> snapshot() override {
        std::unique_lock lock(gate_->mutex);
        gate_->entered = true;
        gate_->ready.notify_all();
        gate_->ready.wait(lock, [&] { return gate_->released; });
        return Result<Observation<SocketRecord>>::success({{listener(32123)}, true, {}});
    }
private:
    std::shared_ptr<SnapshotGate> gate_;
};
void worker_command_queue_is_bounded() {
    const auto gate = std::make_shared<SnapshotGate>();
    Monitor monitor(std::make_unique<FakeProcesses>(), std::make_unique<GatedSockets>(gate));
    struct ReleaseBeforeJoin { std::shared_ptr<SnapshotGate> gate; ~ReleaseBeforeJoin() { gate->release(); } } release{gate};
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->ready.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }));
    }
    for (unsigned i = 0; i < 32; ++i) REQUIRE(monitor.request_refresh());
    CHECK(!monitor.request_refresh());
    gate->release();
    REQUIRE(monitor.wait_for_revision(2, std::chrono::seconds(5)));
    CHECK(monitor.request_refresh());
}
void worker_publishes_immutable_snapshots() {
    Monitor monitor(std::make_unique<FakeProcesses>(), std::make_unique<FakeSockets>());
    REQUIRE(monitor.wait_for_revision(1, std::chrono::seconds(5)));
    const auto first = monitor.view(); REQUIRE(first->services.size() == 1);
    CHECK(first->complete);
    REQUIRE(monitor.request_selection(first->services[0].service_id, first->services[0].endpoints[0].local));
    REQUIRE(wait_view(monitor, [](const auto& view) {
        return view.services.size() == 1 && view.services[0].share_state == ShareState::Selected;
    }));
    CHECK_EQ(first->services[0].share_state, ShareState::Detected); // ancienne vue indépendante.
}
}
int main() {
    CASE(result_invariants); CASE(exact_classifier_and_ancestry); CASE(cycles_missing_and_reused_pid);
    CASE(registry_grouping_and_no_auto_share); CASE(registry_churn_and_incomplete); CASE(unknown_owner_cannot_retain_selection);
    CASE(state_expiry_and_cleanup); CASE(closing_revokes_without_waiting_for_expiry); CASE(roles_contexts_control_and_revocation);
    CASE(five_model_guests_and_capacity); CASE(messages_dm_privacy_and_read_receipts); CASE(messages_quotas_and_feedback_redaction);
    CASE(text_validation); CASE(protocol_golden_and_bounds); CASE(protocol_deterministic_mutations); CASE(estimates_are_bounded);
    CASE(audit_is_opt_in_typed_and_bounded); CASE(unavailable_backends_fail_closed);
    CASE(observation_brackets_process_identity); CASE(observation_keeps_first_snapshot_warning);
    CASE(partial_registry_history_is_bounded); CASE(partial_process_inventory_invalidates_selection);
    CASE(registry_capacity_failure_invalidates_selection); CASE(worker_publishes_immutable_snapshots);
    CASE(worker_invalidates_selection_on_partial_or_error); CASE(worker_command_queue_is_bounded);
    return plpt_test::finish("unit");
}
