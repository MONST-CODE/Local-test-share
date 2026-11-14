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
#include "engine/core/digest.hpp"
#include "engine/core/format.hpp"
#include "engine/core/secrets.hpp"
#include "engine/net/discovery.hpp"
#include "engine/net/proxy.hpp"
#include "engine/net/wire.hpp"

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
    // Protocole 1.1 : même majeur obligatoire, mineur pair inférieur ou égal accepté.
    CHECK(negotiate_version(1, 0)); CHECK(negotiate_version(1, 1));
    CHECK(!negotiate_version(1, 2)); CHECK(!negotiate_version(2, 0)); CHECK(!negotiate_version(0, 1));
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

// ---- Réseau réel : fil de contrôle, secrets, mise en forme, proxy, découverte. ----
void wire_messages_roundtrip_and_guard() {
    using namespace plpt::net;
    GuestHello hello{"Alice", "FRYK-29GY-QASB-6M7H", "PLPT 1.1"};
    auto hello_bytes = encode(hello);
    auto hello_back = decode_guest_hello(hello_bytes); REQUIRE(hello_back);
    CHECK_EQ(hello_back.value().guest_name, hello.guest_name);
    CHECK_EQ(hello_back.value().invite_code, hello.invite_code);
    CHECK_EQ(hello_back.value().client, hello.client);

    HostAuth auth; auth.accepted = true; auth.participant = ParticipantId{7}; auth.host_name = "poste-hôte";
    auth.session_name = "Session démo"; auth.approval_required = true; auth.awaiting_approval = false;
    auto auth_back = decode_host_auth(encode(auth)); REQUIRE(auth_back);
    CHECK(auth_back.value().accepted); CHECK_EQ(auth_back.value().participant.value, 7U);
    CHECK_EQ(auth_back.value().session_name, auth.session_name); CHECK(!auth_back.value().awaiting_approval);
    // « accepté » et « en attente » sont contradictoires : refus explicite.
    HostAuth contradictory = auth; contradictory.awaiting_approval = true;
    CHECK(!decode_host_auth(encode(contradictory)));
    // Un refus en attente d'approbation est un troisième état, pas un échec.
    HostAuth waiting; waiting.accepted = false; waiting.awaiting_approval = true; waiting.host_name = "h"; waiting.session_name = "s";
    auto waiting_back = decode_host_auth(encode(waiting)); REQUIRE(waiting_back);
    CHECK(!waiting_back.value().accepted); CHECK(waiting_back.value().awaiting_approval);

    SessionInfo info; info.session = SessionId{42}; info.session_name = "Session démo"; info.host_name = "poste-hôte";
    info.state = 3; info.max_participants = 5; info.started_ms = 1234;
    info.peers.push_back(PeerInfo{ParticipantId{2}, "Alice", 0, 999, 12, true});
    auto info_back = decode_session_info(encode(info)); REQUIRE(info_back);
    CHECK_EQ(info_back.value().peers.size(), 1U); CHECK(info_back.value().peers[0].self);
    CHECK_EQ(info_back.value().peers[0].latency_ms, 12U);

    ServiceList list{{ServiceInfo{1, "Démo HTTP", 43211, "http://192.168.1.10:43211/?plpt=abcd", true}}, "192.168.1.10:40000"};
    auto list_back = decode_service_list(encode(list)); REQUIRE(list_back);
    CHECK_EQ(list_back.value().services.size(), 1U);
    CHECK_EQ(list_back.value().services[0].exposed_port, 43211);
    CHECK_EQ(list_back.value().control_address, list.control_address);

    ChatText chat; chat.sender = ParticipantId{2}; chat.recipient = ParticipantId{3}; chat.sender_name = "Alice";
    chat.body = "bonjour émoji 🎉"; chat.sent_ms = 55; chat.system = false;
    auto chat_back = decode_chat(encode(chat)); REQUIRE(chat_back);
    CHECK_EQ(chat_back.value().body, chat.body); CHECK_EQ(chat_back.value().recipient.value, 3U);
    ChatText self_dm = chat; self_dm.recipient = chat.sender;
    CHECK(!decode_chat(encode(self_dm))); // expéditeur = destinataire privé : refusé.

    ByeNotice bye{ErrorCode::PermissionDenied, "accès révoqué par l'hôte"};
    auto bye_back = decode_bye(encode(bye)); REQUIRE(bye_back);
    CHECK(bye_back.value().reason == ErrorCode::PermissionDenied);
    CHECK_EQ(bye_back.value().text, bye.text);
    ErrorNotice notice{ErrorCode::RateLimited, "quota atteint"};
    auto notice_back = decode_error(encode(notice)); REQUIRE(notice_back);
    CHECK(notice_back.value().reason == ErrorCode::RateLimited);
    Heartbeat beat{1000, 7};
    auto beat_back = decode_heartbeat(encode(beat)); REQUIRE(beat_back);
    CHECK_EQ(beat_back.value().monotonic_ms, 1000U); CHECK_EQ(beat_back.value().echo, 7U);

    // Trame : réassemblage correct à CHAQUE point de coupure du flux TCP.
    const auto payload = encode(chat);
    auto framed = frame(MessageType::Chat, SessionId{42}, ParticipantId{2}, 9, payload); REQUIRE(framed);
    std::size_t total = 0; Error reason{};
    CHECK(inspect_frame(framed.value(), total, reason) == FrameStatus::Complete);
    CHECK_EQ(total, framed.value().size());
    CHECK(inspect_frame(std::span(framed.value()).first(10), total, reason) == FrameStatus::Incomplete);
    for (std::size_t cut = 1; cut < framed.value().size(); ++cut) {
        FrameAssembler assembler;
        REQUIRE(assembler.feed(std::span(framed.value()).first(cut)));
        auto partial = assembler.next(); CHECK(!partial); CHECK(partial.error().code == ErrorCode::WouldBlock);
        REQUIRE(assembler.feed(std::span(framed.value()).subspan(cut)));
        auto whole = assembler.next(); REQUIRE(whole);
        CHECK(whole.value().type == MessageType::Chat); CHECK_EQ(whole.value().sequence, 9U);
        CHECK_EQ(whole.value().payload, payload);
        auto drained = assembler.next(); CHECK(!drained); CHECK(drained.error().code == ErrorCode::WouldBlock);
    }
    // Gardes : magique, version, taille déclarée, octets excédentaires.
    // feed() accumule (borné) ; c'est next() qui refuse une trame invalide.
    auto bad_magic = framed.value(); bad_magic[0] = 'X';
    FrameAssembler guard; REQUIRE(guard.feed(bad_magic));
    auto magic_rejected = guard.next(); CHECK(!magic_rejected); CHECK_EQ(guard.buffered(), 0U); // tampon purgé.
    auto bad_version = framed.value(); bad_version[5] = 9; // mineur inconnu.
    FrameAssembler guard2; REQUIRE(guard2.feed(bad_version));
    CHECK(!guard2.next());
    auto bad_type = framed.value(); bad_type[9] = 200; // type inconnu => taille 0.
    FrameAssembler guard4; REQUIRE(guard4.feed(bad_type));
    CHECK(!guard4.next());
    auto truncated = framed.value(); truncated.resize(truncated.size() - 1);
    FrameAssembler guard3; REQUIRE(guard3.feed(truncated));
    auto missing = guard3.next(); CHECK(!missing);
    // Des octets suivants restent en attente : jamais interprétés comme la trame courante.
    FrameAssembler tail; auto doubled = framed.value(); doubled.insert(doubled.end(), framed.value().begin(), framed.value().end());
    REQUIRE(tail.feed(doubled));
    REQUIRE(tail.next()); REQUIRE(tail.next()); CHECK_EQ(tail.buffered(), 0U);
    // Bornes de texte : nom trop long et caractère de contrôle refusés.
    CHECK(!sanitize_name(std::string(kMaxNameBytes + 1, 'a'), kMaxNameBytes));
    CHECK(!sanitize_name("nom\x01", kMaxNameBytes));
    CHECK(!sanitize_name("   ", kMaxNameBytes));
    auto clean = sanitize_name("  Alice  ", kMaxNameBytes); REQUIRE(clean); CHECK_EQ(clean.value(), std::string("Alice"));
    CHECK(!sanitize_chat_body(std::string(kMaxChatBytes + 1, 'x')));
    CHECK(!sanitize_chat_body("   "));                       // vide après rognage.
    CHECK(!sanitize_chat_body(std::string("a") + '\0' + "b")); // octet nul refusé.
    CHECK(!sanitize_chat_body("esc[31m"));               // contrôle terminal refusé.
    auto multiline = sanitize_chat_body("line\nbreak");      // multiligne conservé.
    REQUIRE(multiline); CHECK_EQ(multiline.value(), std::string("line\nbreak"));
    auto body = sanitize_chat_body("\r\n bonjour \r\n"); REQUIRE(body); CHECK_EQ(body.value(), std::string("bonjour"));
    auto crlf = sanitize_chat_body("deux\r\nlignes");       // CRLF normalisé en LF.
    REQUIRE(crlf); CHECK_EQ(crlf.value(), std::string("deux\nlignes"));
}

void secrets_digests_and_formats_are_exact() {
    // Empreintes : vecteurs FIPS, fournisseur OS uniquement.
    const auto provider = digest_provider();
    CHECK(provider.available);
    if (provider.available) {
        const std::string abc = "abc";
        const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()};
        auto sha256 = digest(DigestAlgorithm::Sha256, bytes); REQUIRE(sha256);
        CHECK_EQ(to_hex(sha256.value()), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        auto sha1 = digest(DigestAlgorithm::Sha1, bytes); REQUIRE(sha1);
        CHECK_EQ(to_hex(sha1.value()), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
        CHECK_EQ(to_base64(bytes), std::string("YWJj"));
        CHECK(constant_time_equals(sha256.value(), sha256.value()));
        auto other = sha256.value(); other[0] ^= 0xFF;
        CHECK(!constant_time_equals(sha256.value(), other));
    }
    // Codes d'invitation : 80 bits, alphabet sans I/L/O/U, comparaison à temps constant.
    auto generated = generate_invite_code(); REQUIRE(generated);
    const auto code = generated.value();
    CHECK_EQ(code.text.size(), 19U); // 16 symboles + 3 tirets.
    CHECK(invite_code_matches(code.text, code));
    std::string lowered = code.text;
    for (auto& character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    CHECK(invite_code_matches(lowered, code));                       // casse indifférente.
    CHECK(invite_code_matches(code.text.substr(0, 4) + code.text.substr(5), code)); // tirets retirés.
    auto other = generate_invite_code(); REQUIRE(other);
    CHECK(!invite_code_matches(other.value().text, code));
    CHECK(!invite_code_matches("", code));
    CHECK(!invite_code_matches("!!!!-####-@@@@-////", code));
    CHECK(!invite_code_matches(code.text + "A", code));
    auto decoded = decode_invite_code(code.text); REQUIRE(decoded);
    CHECK_EQ(format_invite_code(decoded.value()), code.text);       // aller-retour exact.
    // Le secret n'apparaît jamais en clair dans un journal : seuls les 4 derniers
    // symboles restent visibles, pour que l'hôte puisse vérifier à voix haute.
    const auto masked = mask_secret(code.text);
    CHECK_EQ(masked.size(), code.text.size());
    CHECK_EQ(masked.substr(0, code.text.size() - 4), std::string(code.text.size() - 4, '*'));
    CHECK_EQ(masked.substr(code.text.size() - 4), code.text.substr(code.text.size() - 4));
    CHECK(masked.find(code.text.substr(0, 9)) == std::string::npos); // préfixe jamais visible.
    CHECK_EQ(mask_secret("abc"), std::string("***"));                // court : tout est masqué.
    CHECK_EQ(mask_secret(""), std::string(""));
    // Jetons de service : 32 hexadécimaux, comparaison exacte.
    auto token = generate_service_token(); REQUIRE(token);
    CHECK_EQ(token.value().size(), 32U);
    CHECK(service_token_matches(token.value(), token.value()));
    CHECK(!service_token_matches(token.value(), "00000000000000000000000000000000"));
    CHECK(!service_token_matches("", token.value()));
    CHECK(!service_token_matches(token.value(), ""));
    // Mise en forme : unités binaires exactes, jamais décalées d'un cran.
    CHECK_EQ(format_bytes(0), std::string("0 o"));
    CHECK_EQ(format_bytes(1023), std::string("1023 o"));
    CHECK_EQ(format_bytes(1024), std::string("1.0 Kio"));
    CHECK_EQ(format_bytes(2348), std::string("2.3 Kio"));
    CHECK_EQ(format_bytes(1048576), std::string("1.0 MiB"));
    CHECK_EQ(format_duration_ms(0), std::string("0 ms"));
    CHECK_EQ(format_duration_ms(1500), std::string("1,5 s"));
    CHECK_EQ(format_duration_ms(125000), std::string("2 min 5 s"));
    CHECK_EQ(format_utc(0), std::string("1970-01-01 00:00:00 UTC"));
    CHECK_EQ(format_clock_utc(0), std::string("00:00:00"));
    CHECK_EQ(format_utc(1000), std::string("1970-01-01 00:00:01 UTC"));
}

void proxy_handshake_is_bounded_and_exact() {
    using namespace plpt::net;
    const std::string token = "0123456789abcdef0123456789abcdef";
    constexpr std::size_t kHeadLimit = 8192;
    const auto parse = [&](const std::string& request) {
        return parse_http_head({reinterpret_cast<const std::uint8_t*>(request.data()), request.size()}, kHeadLimit, "plpt");
    };
    {
        const auto request = "GET /?plpt=" + token + " HTTP/1.1\r\nHost: x\r\n\r\n";
        const auto result = parse(request);
        REQUIRE(result); CHECK(result.value().complete); CHECK_EQ(result.value().method, std::string("GET"));
        CHECK_EQ(result.value().token, token);
        CHECK_EQ(result.value().consumed, request.size()); // en-têtes seuls : tout est transmis.
        CHECK(!result.value().upgrade_websocket);
    }
    {   // Jeton par cookie, puis par en-tête dédié.
        const auto cookie = parse("GET / HTTP/1.1\r\nCookie: other=1; plpt=" + token + "\r\n\r\n");
        REQUIRE(cookie); CHECK_EQ(cookie.value().token, token);
        const auto header = parse("GET / HTTP/1.1\r\nX-PLPT-Token: " + token + "\r\n\r\n");
        REQUIRE(header); CHECK_EQ(header.value().token, token);
    }
    {   // Upgrade WebSocket détecté sans interpréter la suite du flux.
        const auto upgrade = parse("GET /ws?plpt=" + token + " HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
        REQUIRE(upgrade); CHECK(upgrade.value().upgrade_websocket); CHECK_EQ(upgrade.value().token, token);
    }
    {   // Incomplet n'est pas invalide : le proxy attend la suite.
        const auto partial = parse("GET /?plpt=" + token + " HTTP/1.1\r\nHost: x\r\n");
        REQUIRE(partial); CHECK(!partial.value().complete); CHECK_EQ(partial.value().consumed, 0U);
    }
    {   // Jeton absent : chaîne vide, jamais un jeton par défaut.
        const auto none = parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        REQUIRE(none); CHECK(none.value().token.empty());
    }
    {   // Ligne de requête absente ou malformée : refus explicite.
        CHECK(!parse("GETONLY\r\n\r\n"));
        CHECK(!parse("\r\n\r\n"));
    }
    {   // Borne stricte : des en-têtes hors limite sont refusés, jamais accumulés.
        const std::string huge = "GET / HTTP/1.1\r\nX-Pad: " + std::string(9000, 'a') + "\r\n\r\n";
        const auto overflow = parse_http_head({reinterpret_cast<const std::uint8_t*>(huge.data()), huge.size()}, 8192, "plpt");
        CHECK(!overflow);
        if (!overflow) CHECK(overflow.error().code == ErrorCode::TooLarge);
    }
    // extract_token : query et cookies, sans se laisser tromper par un préfixe.
    CHECK_EQ(extract_token("/?plpt=abc", "", "plpt"), std::string("abc"));
    CHECK_EQ(extract_token("/?xplpt=abc", "", "plpt"), std::string(""));
    CHECK_EQ(extract_token("/?a=1&plpt=abc&b=2", "", "plpt"), std::string("abc"));
    CHECK_EQ(extract_token("", "Cookie: plpt=abc; other=1\r\n", "plpt"), std::string("abc"));
    CHECK_EQ(extract_token("", "Cookie: nope=abc\r\n", "plpt"), std::string(""));
    // Réponses d'erreur : courtes, sans en-tête de serveur, longueur exacte.
    const auto response = http_status_response(403, "Forbidden", "non");
    const std::string text(response.begin(), response.end());
    CHECK(text.find("HTTP/1.1 403 Forbidden\r\n") == 0);
    CHECK(text.find("Content-Length: 3\r\n") != std::string::npos);
    CHECK(text.find("Connection: close\r\n") != std::string::npos);
    CHECK(text.find("Server:") == std::string::npos);
    CHECK(text.size() > 3 && text.compare(text.size() - 3, 3, "non") == 0);
}

void discovery_announcement_codec_is_bounded() {
    using namespace plpt::net;
    Announcement announcement;
    announcement.session_id = 12345; announcement.session_name = "Session démo"; announcement.host_name = "poste-hôte";
    announcement.control_port = 40000; announcement.participants = 2; announcement.max_participants = 5;
    announcement.shared_services = 1; announcement.accepting = true; announcement.sequence = 7; announcement.wall_ms = 999;
    auto encoded = encode_announcement(announcement); REQUIRE(encoded);
    CHECK(encoded.value().size() <= 512U); // borne datagramme réelle.
    auto decoded = decode_announcement(encoded.value()); REQUIRE(decoded);
    CHECK_EQ(decoded.value().session_id, announcement.session_id);
    CHECK_EQ(decoded.value().session_name, announcement.session_name);
    CHECK_EQ(decoded.value().host_name, announcement.host_name);
    CHECK_EQ(decoded.value().control_port, 40000);
    CHECK_EQ(decoded.value().participants, 2); CHECK_EQ(decoded.value().max_participants, 5);
    CHECK_EQ(decoded.value().shared_services, 1); CHECK(decoded.value().accepting); CHECK_EQ(decoded.value().sequence, 7U);
    // Troncature à chaque longueur : jamais de décodage partiel accepté.
    for (std::size_t cut = 0; cut < encoded.value().size(); ++cut) CHECK(!decode_announcement(std::span(encoded.value()).first(cut)));
    // Octets excédentaires, magique inconnu et champs impossibles : refusés.
    auto extra = encoded.value(); extra.push_back(0);
    CHECK(!decode_announcement(extra));
    auto bad_magic = encoded.value(); bad_magic[0] = 'Z';
    CHECK(!decode_announcement(bad_magic));
    // session_id nul : encodable (l'annonceur ne le connaît pas toujours encore),
    // mais jamais décodable — aucune session réelle ne peut porter l'identifiant 0.
    Announcement no_session = announcement; no_session.session_id = 0;
    auto zero_encoded = encode_announcement(no_session); REQUIRE(zero_encoded);
    CHECK(!decode_announcement(zero_encoded.value()));
    Announcement no_port = announcement; no_port.control_port = 0;
    CHECK(!encode_announcement(no_port));
    Announcement huge_name = announcement; huge_name.session_name = std::string(200, 'x');
    CHECK(!encode_announcement(huge_name));
    Announcement control_char = announcement; control_char.host_name = "hôte\x01";
    CHECK(!encode_announcement(control_char));
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
    CASE(wire_messages_roundtrip_and_guard); CASE(secrets_digests_and_formats_are_exact);
    CASE(proxy_handshake_is_bounded_and_exact); CASE(discovery_announcement_codec_is_bounded);
    return plpt_test::finish("unit");
}
