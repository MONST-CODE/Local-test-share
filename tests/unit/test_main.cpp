#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
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
#include "engine/core/mac.hpp"
#include "engine/core/format.hpp"
#include "engine/core/secrets.hpp"
#include "engine/net/discovery.hpp"
#include "engine/net/proxy.hpp"
#include "engine/net/wire.hpp"
#include "engine/browser/remote_frame.hpp"
#include "engine/media/frame_queue.hpp"
#include "engine/browser/webview2_backend.hpp"
#include "engine/share/media_wire.hpp"
#include "engine/share/share_policy.hpp"
#include "engine/share/view_mapping.hpp"

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
    // Même majeur obligatoire, révision mineure inférieure ou égale acceptée.
    // 1.3 porte l'upgrade TLS en cours de connexion (TlsUpgrade, ADR-016) ;
    // 1.4 n'existe pas et doit être refusé.
    CHECK(negotiate_version(1, 0)); CHECK(negotiate_version(1, 1)); CHECK(negotiate_version(1, 2));
    CHECK(negotiate_version(1, 3));
    CHECK(!negotiate_version(1, 5)); CHECK(!negotiate_version(2, 0)); CHECK(!negotiate_version(0, 1));
    e.payload.resize(8193); CHECK(!encode_envelope(e));
    e = chat(); e.type = static_cast<MessageType>(999); CHECK(!encode_envelope(e));
    e = chat(); e.flags = 1; CHECK(!encode_envelope(e));
    e = chat(); e.participant = {}; CHECK(!encode_envelope(e));
    e = chat(); e.sequence = 0; CHECK(!encode_envelope(e));
}
// ---------------------------------------------------------------------------
// ADR-017 : HMAC-SHA256 (RFC 2104) sur le fournisseur d'empreintes de l'OS.
// Les vecteurs RFC 4231 sont la preuve que la construction est exacte : une
// erreur de construction donnerait des MAC différents de la norme.
// ---------------------------------------------------------------------------
void hmac_sha256_matches_rfc4231() {
    const auto provider = digest_provider();
    REQUIRE(provider.available);   // sans fournisseur, aucune preuve n'est possible.
    struct Vector { std::vector<std::uint8_t> key; std::string data; const char* hex; };
    const std::vector<Vector> vectors = {
        {std::vector<std::uint8_t>(20, 0x0b), "Hi There",
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {std::vector<std::uint8_t>{'J', 'e', 'f', 'e'}, "what do ya want for nothing?",
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {std::vector<std::uint8_t>(20, 0xaa), std::string(50, '\xdd'),
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
        {std::vector<std::uint8_t>(131, 0xaa), "Test Using Larger Than Block-Size Key - Hash Key First",
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {std::vector<std::uint8_t>(131, 0xaa),
            "This is a test using a larger than block-size key and a larger than block-size data. "
            "The key needs to be hashed before being used by the HMAC algorithm.",
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
    };
    for (std::size_t index = 0; index < vectors.size(); ++index) {
        const auto& vector = vectors[index];
        auto mac = hmac_sha256(vector.key,
            {reinterpret_cast<const std::uint8_t*>(vector.data.data()), vector.data.size()});
        REQUIRE(mac);
        CHECK_EQ(mac.value().size(), kHmacSha256Size);
        CHECK_EQ(to_hex(mac.value()), std::string(vector.hex));  // vecteur RFC 4231
    }
    // Clé vide et message vide : définis, pas une erreur silencieuse.
    auto empty = hmac_sha256(std::span<const std::uint8_t>{}, std::span<const std::uint8_t>{});
    REQUIRE(empty);
    CHECK_EQ(to_hex(empty.value()),
        std::string("b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"));
    // Surcharge string_view cohérente avec la surcharge binaire.
    auto by_string = hmac_sha256(std::string_view("clé"), std::string_view("message"));
    REQUIRE(by_string);
    const std::string key = "clé";
    const std::string message = "message";
    auto by_span = hmac_sha256({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()},
        {reinterpret_cast<const std::uint8_t*>(message.data()), message.size()});
    REQUIRE(by_span);
    CHECK(mac_equals(by_string.value(), by_span.value()));
    // Comparaison à temps constant : une seule différence invalide la preuve.
    auto altered = by_string.value();
    altered[altered.size() - 1] ^= 0x01;
    CHECK(!mac_equals(by_string.value(), altered));
    CHECK(!mac_equals(by_string.value(), std::span<const std::uint8_t>(by_string.value()).first(31)));
}

// ---------------------------------------------------------------------------
// ADR-017 : défi-réponse du protocole 1.3. Aucun secret ne transite ; les
// preuves sont liées au nonce, à la session et à la nature du secret.
// ---------------------------------------------------------------------------
void challenge_response_codec_and_derivation() {
    using namespace plpt::net;
    auto code = generate_invite_code(); REQUIRE(code);
    auto resume = generate_resume_secret(); REQUIRE(resume);
    std::vector<std::uint8_t> nonce(kAuthNonceBytes);
    SecureRandom random; REQUIRE(random.fill(nonce));
    const std::vector<std::uint8_t> code_bytes(code.value().bytes.begin(), code.value().bytes.end());
    const std::vector<std::uint8_t> resume_bytes(resume.value().begin(), resume.value().end());

    // Défi : aller-retour exact, nonce public de taille imposée.
    AuthChallenge challenge; challenge.nonce = nonce;
    auto encoded_challenge = encode_auth_challenge(challenge); REQUIRE(encoded_challenge);
    auto challenge_back = decode_auth_challenge(encoded_challenge.value()); REQUIRE(challenge_back);
    CHECK_EQ(challenge_back.value().nonce, nonce);
    AuthChallenge short_nonce; short_nonce.nonce.assign(nonce.begin(), nonce.begin() + 16);
    CHECK(!encode_auth_challenge(short_nonce));            // encodeur : jamais un nonce tronqué
    auto truncated = encoded_challenge.value(); truncated.pop_back();
    CHECK(!decode_auth_challenge(truncated));              // décodeur : troncature refusée
    auto padded = encoded_challenge.value(); padded.push_back(0);
    CHECK(!decode_auth_challenge(padded));                 // octets surnuméraires refusés

    // Preuve : une ou deux preuves, jamais aucune.
    auto invite_proof = compute_auth_proof(code_bytes, nonce, 4242, AuthProofKind::InviteCode);
    REQUIRE(invite_proof);
    auto resume_proof = compute_auth_proof(resume_bytes, nonce, 4242, AuthProofKind::ResumeSecret);
    REQUIRE(resume_proof);
    CHECK_EQ(invite_proof.value().size(), kAuthProofBytes);
    CHECK(!mac_equals(invite_proof.value(), resume_proof.value()));
    AuthProof both; both.invite_proof = invite_proof.value(); both.resume_proof = resume_proof.value();
    auto encoded_both = encode_auth_proof(both); REQUIRE(encoded_both);
    auto both_back = decode_auth_proof(encoded_both.value()); REQUIRE(both_back);
    CHECK_EQ(both_back.value().invite_proof, invite_proof.value());
    CHECK_EQ(both_back.value().resume_proof, resume_proof.value());
    AuthProof invite_only; invite_only.invite_proof = invite_proof.value();
    auto encoded_invite = encode_auth_proof(invite_only); REQUIRE(encoded_invite);
    auto invite_back = decode_auth_proof(encoded_invite.value()); REQUIRE(invite_back);
    CHECK(invite_back.value().resume_proof.empty());
    AuthProof none;
    CHECK(!encode_auth_proof(none));                       // aucune preuve = refus
    auto tampered = encoded_invite.value(); tampered.pop_back();
    CHECK(!decode_auth_proof(tampered));
    // Une preuve de mauvaise taille est refusée à l'encodage comme au décodage.
    AuthProof bogus; bogus.invite_proof.assign(31, 0x5a);
    CHECK(!encode_auth_proof(bogus));

    // La preuve dépend du nonce, de la session et de la nature du secret :
    // aucune réutilisation possible d'une capture.
    std::vector<std::uint8_t> other_nonce(kAuthNonceBytes, 0x11);
    auto other = compute_auth_proof(code_bytes, other_nonce, 4242, AuthProofKind::InviteCode);
    REQUIRE(other);
    CHECK(!mac_equals(other.value(), invite_proof.value()));
    auto other_session = compute_auth_proof(code_bytes, nonce, 4243, AuthProofKind::InviteCode);
    REQUIRE(other_session);
    CHECK(!mac_equals(other_session.value(), invite_proof.value()));
    auto other_kind = compute_auth_proof(code_bytes, nonce, 4242, AuthProofKind::ResumeSecret);
    REQUIRE(other_kind);
    CHECK(!mac_equals(other_kind.value(), invite_proof.value()));
    // Le message authentifié est explicite et borné.
    const auto message = auth_proof_message(nonce, 4242, AuthProofKind::InviteCode);
    CHECK_EQ(message.size(), kAuthNonceBytes + 9);
    CHECK_EQ(message.back(), static_cast<std::uint8_t>(AuthProofKind::InviteCode));
    // Nonce hors taille : refus, jamais de preuve sur un contexte faux.
    CHECK(!compute_auth_proof(code_bytes, std::span<const std::uint8_t>(nonce).first(8), 4242,
        AuthProofKind::InviteCode));
    // Secret vide : refus explicite (pas de MAC sur clé vide silencieuse).
    CHECK(!compute_auth_proof(std::span<const std::uint8_t>{}, nonce, 4242, AuthProofKind::InviteCode));

    // Notice d'upgrade : publique, sans secret, liée au nonce vérifié.
    TlsUpgradeNotice notice;
    notice.identity = "dl-s4242-abcd";
    notice.secret_kind = AuthProofKind::ResumeSecret;
    notice.nonce = nonce;
    auto encoded_notice = encode_tls_upgrade(notice); REQUIRE(encoded_notice);
    auto notice_back = decode_tls_upgrade(encoded_notice.value()); REQUIRE(notice_back);
    CHECK_EQ(notice_back.value().identity, notice.identity);
    CHECK(notice_back.value().secret_kind == AuthProofKind::ResumeSecret);
    CHECK_EQ(notice_back.value().nonce, nonce);
    TlsUpgradeNotice empty_identity = notice; empty_identity.identity.clear();
    CHECK(!encode_tls_upgrade(empty_identity));
    TlsUpgradeNotice control_identity = notice; control_identity.identity = "dl\ns7";
    CHECK(!encode_tls_upgrade(control_identity));
    TlsUpgradeNotice long_identity = notice; long_identity.identity.assign(129, 'a');
    CHECK(!encode_tls_upgrade(long_identity));
    TlsUpgradeNotice wrong_nonce = notice; wrong_nonce.nonce.assign(kAuthNonceBytes - 1, 0x22);
    CHECK(!encode_tls_upgrade(wrong_nonce));
    auto notice_truncated = encoded_notice.value(); notice_truncated.pop_back();
    CHECK(!decode_tls_upgrade(notice_truncated));

    // Secret de transport : dérivé localement, identique des deux côtés, lié au
    // contexte, et jamais égal au secret ni à sa simple empreinte.
    auto host_secret = derive_transport_secret(code_bytes, nonce, 4242, notice.identity);
    REQUIRE(host_secret);
    auto guest_secret = derive_transport_secret(code_bytes, nonce, 4242, notice.identity);
    REQUIRE(guest_secret);
    CHECK_EQ(host_secret.value(), guest_secret.value());
    CHECK(host_secret.value().size() >= 16);               // borne de la fabrique TLS
    CHECK_EQ(host_secret.value().size(), 64U);             // hexadécimal de 32 octets
    for (const char digit : host_secret.value())
        CHECK((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'));
    CHECK(host_secret.value() != to_hex(code_bytes));
    auto other_identity = derive_transport_secret(code_bytes, nonce, 4242, "dl-s4242-autre");
    REQUIRE(other_identity);
    CHECK(other_identity.value() != host_secret.value());
    auto other_nonce_secret = derive_transport_secret(code_bytes, other_nonce, 4242, notice.identity);
    REQUIRE(other_nonce_secret);
    CHECK(other_nonce_secret.value() != host_secret.value());
    auto resume_secret_derived = derive_transport_secret(resume_bytes, nonce, 4242, notice.identity);
    REQUIRE(resume_secret_derived);
    CHECK(resume_secret_derived.value() != host_secret.value());
    CHECK(!derive_transport_secret(code_bytes, std::span<const std::uint8_t>(nonce).first(4), 4242,
        notice.identity));
    CHECK(!derive_transport_secret(std::span<const std::uint8_t>{}, nonce, 4242, notice.identity));
    CHECK(!derive_transport_secret(code_bytes, nonce, 4242, std::string()));

    // Trames d'AVANT admission (ADR-017) : participant nul accepté pour le défi,
    // la preuve et la notice d'upgrade ; refusé pour le reste du protocole, qui
    // exige une place attribuée. Séquence et session restent obligatoires :
    // aucune de ces trames n'est rejouable.
    auto framed_proof = frame(MessageType::AuthProof, SessionId{4242}, ParticipantId{}, 1, encoded_invite.value());
    REQUIRE(framed_proof);
    auto framed_notice = frame(MessageType::TlsUpgrade, SessionId{4242}, ParticipantId{}, 1, encoded_notice.value());
    REQUIRE(framed_notice);
    CHECK(!frame(MessageType::Chat, SessionId{4242}, ParticipantId{}, 1, encoded_challenge.value()));
    CHECK(!frame(MessageType::ServiceList, SessionId{4242}, ParticipantId{}, 1, encoded_challenge.value()));
    CHECK(!frame(MessageType::AuthChallenge, SessionId{4242}, ParticipantId{}, 0, encoded_challenge.value()));
    CHECK(!frame(MessageType::AuthProof, SessionId{4242}, ParticipantId{}, 0, encoded_invite.value()));
    CHECK(!frame(MessageType::TlsUpgrade, SessionId{4242}, ParticipantId{}, 0, encoded_notice.value()));
    CHECK(!frame(MessageType::AuthChallenge, SessionId{}, ParticipantId{}, 1, encoded_challenge.value()));
    CHECK(!frame(MessageType::TlsUpgrade, SessionId{}, ParticipantId{}, 1, encoded_notice.value()));

    // Les nouvelles trames respectent les bornes de charge utile du protocole.
    CHECK(max_payload_size(MessageType::AuthChallenge) >= encoded_challenge.value().size());
    CHECK(max_payload_size(MessageType::AuthProof) >= encoded_both.value().size());
    CHECK(max_payload_size(MessageType::TlsUpgrade) >= encoded_notice.value().size());
    auto framed_challenge = frame(MessageType::AuthChallenge, SessionId{4242}, ParticipantId{}, 1,
        encoded_challenge.value());
    REQUIRE(framed_challenge);
    auto framed_back = decode_envelope(framed_challenge.value()); REQUIRE(framed_back);
    CHECK(framed_back.value().type == MessageType::AuthChallenge);
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
void demo_browser_isolation_and_allowlist() {
    DemoBrowserBackend backend;
    BrowserPolicy policy;
    policy.allowed_origins.push_back("http://127.0.0.1:8080");
    policy.allowed_origins.push_back("http://localhost:3000");

    // Invalid participant
    CHECK(!backend.create_context(ParticipantId{0}, policy));

    // Valid participant 1
    auto c1 = backend.create_context(ParticipantId{1}, policy);
    REQUIRE(c1);
    CHECK(c1.value().context.value != 0);
    CHECK_EQ(c1.value().owner, ParticipantId{1});
    CHECK(!c1.value().profile_id.empty());

    // Policy origin check
    CHECK(!policy.is_origin_allowed(""));
    CHECK(!policy.is_origin_allowed("file:///C:/Windows/win.ini"));
    CHECK(!policy.is_origin_allowed("javascript:alert(1)"));
    CHECK(!policy.is_origin_allowed("data:text/html,bad"));
    CHECK(!policy.is_origin_allowed("http://malicious-site.com"));
    CHECK(!policy.is_origin_allowed("http://127.0.0.1:8080.attacker.example/dashboard"));
    CHECK(!policy.is_origin_allowed("http://127.0.0.1:8080@attacker.example/dashboard"));
    CHECK(!policy.is_origin_allowed("http://127.0.0.1:8080\\@attacker.example/dashboard"));
    CHECK(!policy.is_origin_allowed("http://127.0.0.1:8080.evil/dashboard"));
    CHECK(!policy.is_origin_allowed("http://0x7f000001:8080/dashboard"));
    CHECK(!policy.is_origin_allowed("http://127.1:8080/dashboard"));
    CHECK(!policy.is_origin_allowed("http://0177.0.0.1:8080/dashboard"));
    CHECK(policy.is_origin_allowed("HTTP://127.0.0.1:08080/dashboard"));
    CHECK(policy.is_origin_allowed("http://127.0.0.1:8080/dashboard"));
    CHECK(policy.is_origin_allowed("http://localhost:3000/api"));
    CHECK(policy.is_origin_allowed("http://LOCALHOST:3000/api"));
    CHECK(!policy.is_origin_allowed("http://localhost:3001/api"));
    BrowserPolicy wildcard;
    wildcard.allowed_origins.push_back("*");
    CHECK(!wildcard.is_origin_allowed("http://127.0.0.1:8080/"));
    BrowserPolicy ipv6;
    ipv6.allowed_origins.push_back("http://[::1]:8080");
    CHECK(ipv6.is_origin_allowed("http://[0:0:0:0:0:0:0:1]:8080/page"));

    // Navigation security
    CHECK(!backend.navigate(c1.value().context, "file:///etc/passwd"));
    CHECK(!backend.navigate(c1.value().context, "http://192.168.1.50:9000"));
    REQUIRE(backend.navigate(c1.value().context, "http://127.0.0.1:8080/app"));

    // Frame capture
    auto frame = backend.capture_frame(c1.value().context);
    REQUIRE(frame);
    CHECK(frame.value().size() >= 8 + 128 * 64 * 4);
    CHECK_EQ(frame.value()[0], 0); // width high
    CHECK_EQ(frame.value()[1], 128); // width low
    CHECK_EQ(frame.value()[2], 0); // height high
    CHECK_EQ(frame.value()[3], 64); // height low

    // Input injection
    CHECK(!backend.inject_input(c1.value().context, {}));
    std::vector<std::uint8_t> click_event = {0x01, 0x10, 0x20};
    REQUIRE(backend.inject_input(c1.value().context, click_event));

    // Distinct context 2
    auto c2 = backend.create_context(ParticipantId{2}, policy);
    REQUIRE(c2);
    CHECK(c2.value().context != c1.value().context);
    CHECK(c2.value().profile_id != c1.value().profile_id);
    CHECK_EQ(backend.active_contexts(), 2U);

    // Context destruction
    REQUIRE(backend.destroy_context(c1.value().context));
    CHECK_EQ(backend.active_contexts(), 1U);
    CHECK(!backend.capture_frame(c1.value().context)); // ctx1 is destroyed
    REQUIRE(backend.destroy_context(c2.value().context));
    CHECK_EQ(backend.active_contexts(), 0U);
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
    GuestHello hello{"Alice", "FRYK-29GY-QASB-6M7H", "PLPT 1.2", ""}; // 4e champ : secret de reprise.
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
    // ADR-016 : le transport RÉEL de la session et de chaque pair est annoncé.
    info.transport = "TLS 1.3 PSK (pairs authentifiés)";
    info.peers.push_back(PeerInfo{ParticipantId{2}, "Alice", 0, 999, 12, true,
        "TLSv1.3 TLS_AES_128_GCM_SHA256 (pair authentifié)"});
    auto info_back = decode_session_info(encode(info)); REQUIRE(info_back);
    CHECK_EQ(info_back.value().peers.size(), 1U); CHECK(info_back.value().peers[0].self);
    CHECK_EQ(info_back.value().peers[0].latency_ms, 12U);
    CHECK_EQ(info_back.value().transport, info.transport);
    CHECK_EQ(info_back.value().peers[0].transport, info.peers[0].transport);
    // Bloc 1.3 vide : encodage court, décodage identique (les pairs 1.2 aussi).
    SessionInfo plain = info; plain.transport.clear(); plain.peers[0].transport.clear();
    auto plain_back = decode_session_info(encode(plain)); REQUIRE(plain_back);
    CHECK(plain_back.value().transport.empty());
    CHECK(plain_back.value().peers[0].transport.empty());
    CHECK_EQ(plain_back.value().peers[0].name, std::string("Alice"));

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


void capability_is_never_forwarded_upstream() {
    using namespace plpt::net;
    const std::string token = "0123456789abcdef0123456789abcdef";
    const auto redact = [&](const std::string& head) {
        const auto out = redact_capability({reinterpret_cast<const std::uint8_t*>(head.data()), head.size()}, "plpt");
        CHECK(out.size() <= head.size()); // aucune inflation possible.
        return std::string(out.begin(), out.end());
    };
    {   // Paramètre de capacité retiré, les autres paramètres conservés dans l'ordre.
        CHECK_EQ(redact("GET /app?a=1&plpt=" + token + "&b=2 HTTP/1.1\r\nHost: x\r\n\r\n"),
            std::string("GET /app?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n"));
        // Paramètre unique : la cible redevient le chemin seul, sans « ? » vide.
        CHECK_EQ(redact("GET /?plpt=" + token + " HTTP/1.1\r\nHost: x\r\n\r\n"),
            std::string("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
    }
    {   // Cookie de capacité : retiré seul, la ligne disparaît s'il ne reste rien.
        CHECK_EQ(redact("GET / HTTP/1.1\r\nCookie: theme=dark; plpt=" + token + "\r\nAccept: */*\r\n\r\n"),
            std::string("GET / HTTP/1.1\r\nCookie: theme=dark\r\nAccept: */*\r\n\r\n"));
        CHECK_EQ(redact("GET / HTTP/1.1\r\nCookie: plpt=" + token + "\r\n\r\n"),
            std::string("GET / HTTP/1.1\r\n\r\n"));
    }
    {   // En-tête dédié et Referer : aucune trace du jeton ne traverse le proxy.
        const auto out = redact("GET / HTTP/1.1\r\nX-PLPT-Token: " + token + "\r\nReferer: http://hote/?plpt=" + token + "\r\n\r\n");
        CHECK(out.find("X-PLPT-Token") == std::string::npos);
        CHECK(out.find(token) == std::string::npos);
        CHECK(out.find("Referer: http://hote/\r\n") != std::string::npos);
    }
    {   // Sans capacité : l'en-tête est reproduit octet pour octet, jamais réécrit.
        const std::string plain = "POST /api HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
        CHECK_EQ(redact(plain), plain);
        // Un nom homonyme n'est pas touché : seule la clé exacte est retirée.
        const std::string lookalike = "GET /?xplpt=abc&plpt2=def HTTP/1.1\r\nHost: x\r\n\r\n";
        CHECK_EQ(redact(lookalike), lookalike);
    }
    // Cookie de capacité émis par PLPT : session, HttpOnly, SameSite, jamais persistant.
    const auto line = capability_cookie_line("plpt", token);
    CHECK(line.find("Set-Cookie: plpt=" + token) == 0);
    CHECK(line.find("Path=/") != std::string::npos);
    CHECK(line.find("HttpOnly") != std::string::npos);
    CHECK(line.find("SameSite=Strict") != std::string::npos);
    CHECK(line.find("Max-Age") == std::string::npos);
    CHECK(line.find('\r') == std::string::npos);
    CHECK(line.find('\n') == std::string::npos);
    {   // Insertion dans la première réponse : avant la ligne vide, corps intact.
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
        const auto terminator = response.find("\r\n\r\n");
        REQUIRE(terminator != std::string::npos);
        const auto out = inject_response_header(
            {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, terminator, line);
        const std::string text(out.begin(), out.end());
        CHECK_EQ(text, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n" + line + "\r\n\r\nabc");
        CHECK_EQ(text.size(), response.size() + line.size() + 2);
    }
    {   // Provenance de la capacité : cookie ou URL, distinguée sans ambiguïté.
        const std::string via_cookie = "GET / HTTP/1.1\r\nCookie: plpt=" + token + "\r\n\r\n";
        const auto parsed = parse_http_head({reinterpret_cast<const std::uint8_t*>(via_cookie.data()), via_cookie.size()}, 8192, "plpt");
        REQUIRE(parsed); CHECK(parsed.value().token_via_cookie);
        const std::string via_url = "GET /?plpt=" + token + " HTTP/1.1\r\n\r\n";
        const auto other = parse_http_head({reinterpret_cast<const std::uint8_t*>(via_url.data()), via_url.size()}, 8192, "plpt");
        REQUIRE(other); CHECK(!other.value().token_via_cookie);
        const std::string via_header = "GET / HTTP/1.1\r\nX-PLPT-Token: " + token + "\r\n\r\n";
        const auto third = parse_http_head({reinterpret_cast<const std::uint8_t*>(via_header.data()), via_header.size()}, 8192, "plpt");
        REQUIRE(third); CHECK(!third.value().token_via_cookie);
    }
}

void bind_resolution_never_defaults_to_any() {
    {   // Choix explicite : conservé tel quel, et signalé comme un choix.
        const auto chosen = resolve_bind_address(IpAddress::loopback(IpFamily::Ipv4), IpFamily::Ipv4);
        REQUIRE(chosen);
        CHECK(chosen.value().explicit_choice);
        CHECK(!chosen.value().any_interface);
        CHECK_EQ(chosen.value().address.text, std::string("127.0.0.1"));
        // Toutes les interfaces restent possibles, mais jamais en silence.
        const auto wide = resolve_bind_address(IpAddress::any(IpFamily::Ipv4), IpFamily::Ipv4);
        REQUIRE(wide);
        CHECK(wide.value().any_interface);
        CHECK(wide.value().note.find("explicite") != std::string::npos);
    }
    {   // Adresse non précisée : PLPT choisit une adresse réelle, jamais 0.0.0.0.
        const auto resolved = resolve_bind_address(IpAddress{}, IpFamily::Ipv4);
        REQUIRE(resolved);
        CHECK(!resolved.value().explicit_choice);
        CHECK(!resolved.value().any_interface);
        CHECK(resolved.value().address.valid());
        CHECK(resolved.value().address.is_private_lan());
        CHECK(!resolved.value().note.empty());
        if (resolved.value().loopback_only) CHECK(resolved.value().address.is_loopback());
        else CHECK(!resolved.value().address.is_loopback());
    }
    {   // Énumération réelle : adresses lisibles, boucle locale présente et marquée.
        const auto interfaces = enumerate_network_interfaces();
        REQUIRE(interfaces);
        bool saw_loopback = false, saw_up = false;
        for (const auto& info : interfaces.value()) {
            CHECK(!info.name.empty());
            CHECK(!info.address.empty());
            CHECK(static_cast<bool>(IpAddress::parse(info.address)));
            if (info.is_loopback) saw_loopback = true;
            if (info.is_up) saw_up = true;
        }
        CHECK(saw_loopback);
        CHECK(saw_up);
        // La sélection LAN ne retient jamais la boucle locale ni une interface down.
        for (const auto index : select_lan_interfaces(interfaces.value())) {
            CHECK(!interfaces.value()[index].is_loopback);
            CHECK(interfaces.value()[index].is_up);
        }
    }
}

void resume_secret_is_the_only_proof_of_a_place() {
    using namespace plpt::net;
    {   // Hello 1.2 : le secret de reprise traverse, octet pour octet.
        GuestHello hello{"Alice", "FRYK-29GY-QASB-6M7H", "PLPT 1.2", "0123456789abcdef0123456789abcdef"};
        const auto payload = encode(hello);
        const auto decoded = decode_guest_hello(payload);
        REQUIRE(decoded);
        CHECK_EQ(decoded.value().guest_name, std::string("Alice"));
        CHECK_EQ(decoded.value().resume_secret, hello.resume_secret);
        // Champ obligatoire : un Hello amputé du secret est refusé, pas deviné.
        REQUIRE(payload.size() > hello.resume_secret.size() + 2);
        const std::span<const std::uint8_t> truncated(payload.data(), payload.size() - hello.resume_secret.size() - 2);
        CHECK(!decode_guest_hello(truncated));
        // Un secret porteur d'un caractère de contrôle est refusé.
        GuestHello dirty = hello;
        dirty.resume_secret = std::string("abc") + static_cast<char>(0x01);
        CHECK(!decode_guest_hello(encode(dirty)));
    }
    {   // Auth : le secret et sa durée de validité sont délivrés ensemble.
        HostAuth auth;
        auth.accepted = true;
        auth.participant = ParticipantId{7};
        auth.host_name = "poste-hôte";
        auth.session_name = "Session démo";
        auth.resume_secret = "fedcba9876543210fedcba9876543210";
        auth.resume_ttl_ms = 30000;
        const auto decoded = decode_host_auth(encode(auth));
        REQUIRE(decoded);
        CHECK_EQ(decoded.value().resume_secret, auth.resume_secret);
        CHECK_EQ(decoded.value().resume_ttl_ms, 30000U);
        // Aucun secret : le champ reste vide et la durée nulle, jamais une valeur par défaut.
        HostAuth none = auth;
        none.resume_secret.clear();
        none.resume_ttl_ms = 0;
        const auto back = decode_host_auth(encode(none));
        REQUIRE(back);
        CHECK(back.value().resume_secret.empty());
        CHECK_EQ(back.value().resume_ttl_ms, 0U);
    }
    {   // 200 secrets successifs : tous distincts, longueur fixe, hexadécimaux.
        std::vector<std::string> seen;
        seen.reserve(200);
        for (int index = 0; index < 200; ++index) {
            const auto secret = generate_resume_secret();
            REQUIRE(secret);
            CHECK_EQ(secret.value().size(), kResumeSecretChars);
            for (const char character : secret.value())
                CHECK((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'));
            CHECK(std::find(seen.begin(), seen.end(), secret.value()) == seen.end());
            seen.push_back(secret.value());
        }
        CHECK_EQ(seen.size(), 200U);
    }
}

void remote_frame_and_input_serialization() {
    using namespace plpt::media;
    VideoFrame frame;
    frame.header.session = SessionId{0x0102030405060708ULL};
    frame.header.participant = ParticipantId{9};
    frame.header.stream_id = 3;
    frame.header.context = ContextId{11};
    frame.header.sequence = 42;
    frame.header.timestamp_ns = 1337;
    frame.header.width = 2;
    frame.header.height = 1;
    frame.header.stride_bytes = 8;
    frame.pixels = {10, 20, 30, 40, 50, 60, 70, 80};

    auto enc_frame = serialize_video_frame(frame);
    REQUIRE(enc_frame);
    CHECK_EQ(enc_frame.value().size(), kFrameHeaderBytes + frame.pixels.size());
    CHECK_EQ(enc_frame.value()[0], static_cast<std::uint8_t>('D'));
    CHECK_EQ(enc_frame.value()[1], static_cast<std::uint8_t>('L'));
    CHECK_EQ(enc_frame.value()[2], static_cast<std::uint8_t>('V'));
    CHECK_EQ(enc_frame.value()[3], static_cast<std::uint8_t>('F'));
    auto dec_frame = deserialize_video_frame(enc_frame.value());
    REQUIRE(dec_frame);
    CHECK_EQ(dec_frame.value().header.session.value, frame.header.session.value);
    CHECK_EQ(dec_frame.value().header.participant.value, 9ULL);
    CHECK_EQ(dec_frame.value().header.stream_id, 3U);
    CHECK_EQ(dec_frame.value().header.context.value, 11ULL);
    CHECK_EQ(dec_frame.value().header.width, 2U);
    CHECK_EQ(dec_frame.value().header.height, 1U);
    CHECK_EQ(dec_frame.value().header.stride_bytes, 8U);
    CHECK_EQ(dec_frame.value().header.sequence, 42ULL);
    CHECK_EQ(dec_frame.value().header.timestamp_ns, 1337ULL);
    CHECK_EQ(dec_frame.value().pixels.size(), 8U);
    CHECK_EQ(dec_frame.value().pixels[0], 10);
    CHECK_EQ(dec_frame.value().pixels[7], 80);

    auto truncated_frame = deserialize_video_frame(std::span<const std::uint8_t>(enc_frame.value()).first(kFrameHeaderBytes - 1));
    CHECK(!truncated_frame);
    auto extra_frame_bytes = enc_frame.value();
    extra_frame_bytes.push_back(0);
    CHECK(!deserialize_video_frame(extra_frame_bytes));
    auto wrong_magic = enc_frame.value();
    wrong_magic[0] = 0;
    CHECK(!deserialize_video_frame(wrong_magic));
    auto wrong_stride = enc_frame.value();
    wrong_stride[67] = 7; // stride field is big-endian at offsets 64..67
    CHECK(!deserialize_video_frame(wrong_stride));
    auto unsupported_codec = enc_frame.value();
    unsupported_codec[8] = 99;
    CHECK(!deserialize_video_frame(unsupported_codec));
    auto unsupported_version = enc_frame.value();
    unsupported_version[5] = 99;
    auto bad_version = deserialize_video_frame(unsupported_version);
    CHECK(!bad_version);
    if (!bad_version) CHECK_EQ(bad_version.error().code, ErrorCode::UnsupportedVersion);

    InputEvent ev;
    ev.type = InputEventType::MouseDown;
    ev.x_norm = 0.5f;
    ev.y_norm = 0.25f;
    ev.button = 1;
    ev.modifiers = 2;

    auto enc_ev = serialize_input_event(ev);
    REQUIRE(enc_ev);
    auto dec_ev = deserialize_input_event(enc_ev.value());
    REQUIRE(dec_ev);
    CHECK_EQ(static_cast<int>(dec_ev.value().type), static_cast<int>(InputEventType::MouseDown));
    CHECK(std::abs(dec_ev.value().x_norm - 0.5f) < 0.00002f);
    CHECK(std::abs(dec_ev.value().y_norm - 0.25f) < 0.00002f);
    CHECK_EQ(dec_ev.value().button, 1);
    CHECK_EQ(dec_ev.value().modifiers, 2);
    CHECK_EQ(dec_ev.value().wheel_delta, 0);
    CHECK_EQ(dec_ev.value().key_code, 0U);
    CHECK(dec_ev.value().text.empty());

    InputEvent text;
    text.type = InputEventType::TextInput;
    text.text = "Café";
    auto encoded_text = serialize_input_event(text);
    REQUIRE(encoded_text);
    auto decoded_text = deserialize_input_event(encoded_text.value());
    REQUIRE(decoded_text);
    CHECK_EQ(decoded_text.value().text, text.text);

    InputEvent invalid = ev;
    invalid.x_norm = std::numeric_limits<float>::quiet_NaN();
    CHECK(!serialize_input_event(invalid));
    invalid = text;
    invalid.text = std::string("\xC0\xAF", 2); // non-canonical UTF-8
    CHECK(!serialize_input_event(invalid));
    auto trailing_input = encoded_text.value();
    trailing_input.push_back(0);
    CHECK(!deserialize_input_event(trailing_input));
    auto reserved_input = enc_ev.value();
    reserved_input[7] = 1;
    CHECK(!deserialize_input_event(reserved_input));
    auto invalid_type = enc_ev.value();
    invalid_type[5] = 0xff;
    CHECK(!deserialize_input_event(invalid_type));

    InputEvent focus_lost;
    focus_lost.type = InputEventType::FocusLost;
    auto encoded_focus = serialize_input_event(focus_lost);
    REQUIRE(encoded_focus);
    CHECK(deserialize_input_event(encoded_focus.value()));
}

void bounded_frame_queue_enforces_limits_and_lifecycle() {
    using namespace plpt::media;
    auto make_frame = [](std::uint64_t sequence, ContextId context = ContextId{7}) {
        VideoFrame frame;
        frame.header.context = context;
        frame.header.stream_id = 2;
        frame.header.sequence = sequence;
        frame.header.width = 1;
        frame.header.height = 1;
        frame.header.stride_bytes = 4;
        frame.pixels = {static_cast<std::uint8_t>(sequence), 2, 3, 255};
        return frame;
    };

    BoundedFrameQueue queue(3, 8);
    auto first = queue.push(make_frame(1)); REQUIRE(first);
    CHECK_EQ(first.value(), FramePushOutcome::Queued);
    REQUIRE(queue.push(make_frame(2)));
    auto third = queue.push(make_frame(3)); REQUIRE(third);
    CHECK_EQ(third.value(), FramePushOutcome::QueuedAfterDroppingOldest);
    auto snapshot = queue.snapshot();
    CHECK_EQ(snapshot.queued_frames, 2U);
    CHECK_EQ(snapshot.queued_bytes, 8U);
    CHECK_EQ(snapshot.dropped_frames, 1ULL);
    auto second_frame = queue.try_pop(); REQUIRE(second_frame);
    CHECK_EQ(second_frame->header.sequence, 2ULL);
    auto third_frame = queue.try_pop(); REQUIRE(third_frame);
    CHECK_EQ(third_frame->header.sequence, 3ULL);
    CHECK(!queue.try_pop());
    auto replay = queue.push(make_frame(3));
    CHECK(!replay);
    if (!replay) CHECK_EQ(replay.error().code, ErrorCode::Replayed);
    auto wrong_context = queue.push(make_frame(4, ContextId{8}));
    CHECK(!wrong_context);
    if (!wrong_context) CHECK_EQ(wrong_context.error().code, ErrorCode::InvalidArgument);

    BoundedFrameQueue too_small(2, 3);
    auto too_large = too_small.push(make_frame(1));
    CHECK(!too_large);
    if (!too_large) CHECK_EQ(too_large.error().code, ErrorCode::TooLarge);

    BoundedFrameQueue closed(2, 8);
    REQUIRE(closed.push(make_frame(1)));
    closed.close();
    closed.close();
    CHECK(closed.snapshot().closed);
    auto rejected = closed.push(make_frame(2));
    CHECK(!rejected);
    if (!rejected) CHECK_EQ(rejected.error().code, ErrorCode::InvalidState);
    auto drain = closed.try_pop(); REQUIRE(drain);
    CHECK_EQ(drain->header.sequence, 1ULL);
}

void browser_backend_selection_and_lifecycle() {
    // Availability is a runtime fact, never a constant: implemented code AND an
    // installed runtime. The real browser is exercised by devlink-browser-tests;
    // here only the fail-closed contract that needs no browser process.
    CHECK_EQ(plpt::browser::WebView2Backend::is_available(),
        plpt::browser::kWebView2BackendImplemented && plpt::browser::WebView2Backend::runtime_available());
    plpt::browser::WebView2Backend real;
    BrowserPolicy policy;
    policy.allowed_origins.push_back("http://127.0.0.1:3000");
    auto bad_owner = real.create_context(ParticipantId{0}, policy);
    CHECK(!bad_owner);
    if (!bad_owner) CHECK(bad_owner.error().code == ErrorCode::InvalidArgument || bad_owner.error().code == ErrorCode::PlatformError);
    auto no_selection = real.create_context(ParticipantId{7}, BrowserPolicy{});
    CHECK(!no_selection); // no explicit origin, no browser
    BrowserPolicy wildcard;
    wildcard.allowed_origins.push_back("http://*:3000");
    CHECK(!real.create_context(ParticipantId{7}, wildcard));
    auto rejected_frame = real.capture_frame(ContextId{1});
    CHECK(!rejected_frame);
    auto rejected_input = real.inject_input(ContextId{1}, std::vector<std::uint8_t>{1, 2, 3});
    CHECK(!rejected_input); // malformed and unknown context
    CHECK(!real.wait_frame(ContextId{1}, 0, std::chrono::milliseconds(0)));
    CHECK(real.destroy_context(ContextId{1})); // idempotent on unknown
    CHECK_EQ(real.active_contexts(), 0U);
    CHECK_EQ(real.real_engine(), plpt::browser::kWebView2BackendImplemented);
    plpt::DemoBrowserBackend synthetic_check;
    CHECK(!synthetic_check.real_engine()); // the demo backend never claims a real engine
    CHECK(!synthetic_check.wait_frame(ContextId{1}, 0, std::chrono::milliseconds(0)));

    // Le backend géré est utilisé ici comme fixture synthétique de test; il ne
    // constitue jamais une preuve de capture navigateur réelle.
    auto backend = std::make_unique<plpt::DemoBrowserBackend>();
    REQUIRE(backend);

    auto ctx_res = backend->create_context(ParticipantId{7}, policy);
    REQUIRE(ctx_res);
    CHECK_EQ(ctx_res.value().owner.value, 7U);
    CHECK(!ctx_res.value().profile_id.empty());

    auto nav_ok = backend->navigate(ctx_res.value().context, "http://127.0.0.1:3000/shop");
    CHECK(nav_ok);

    auto nav_bad = backend->navigate(ctx_res.value().context, "file:///etc/shadow");
    CHECK(!nav_bad);
    CHECK_EQ(nav_bad.error().code, ErrorCode::PermissionDenied);

    auto nav_ext = backend->navigate(ctx_res.value().context, "http://malicious-external-site.com");
    CHECK(!nav_ext);
    CHECK_EQ(nav_ext.error().code, ErrorCode::PermissionDenied);

    auto frame_res = backend->capture_frame(ctx_res.value().context);
    REQUIRE(frame_res);
    CHECK(!frame_res.value().empty());

    std::vector<std::uint8_t> dummy_input = {1, 2, 3};
    auto inject_res = backend->inject_input(ctx_res.value().context, dummy_input);
    CHECK(inject_res);

    auto destroy_res = backend->destroy_context(ctx_res.value().context);
    CHECK(destroy_res);
}

// ---- engine/share : policy (modes A/B/C), media records, view mapping ----

static void share_policy_modes_are_enforced_server_side() {
    using namespace plpt::share;
    const ParticipantId a{11}, b{12}, c{13};
    // Mode C: valid, fresh records are still refused; no lease ever exists.
    {
        SharePolicy p(ShareMode::ObserveOnly);
        p.set_shared_context(ContextId{7});
        REQUIRE(p.add(a, "A"));
        ContextId target{};
        CHECK(p.authorize(a, 1, &target) == InputDecision::ObserveOnlyMode);
        CHECK(!p.request_control(a));
        CHECK(!p.host_grant(a));
        CHECK(!p.may_input(a));
        CHECK_EQ(target.value, 0U);
    }
    // Mode B: exclusive lease, FIFO transfer, refused after release/revocation.
    {
        SharePolicy p(ShareMode::Collaborative);
        p.set_shared_context(ContextId{7});
        REQUIRE(p.add(a, "A"));
        REQUIRE(p.add(b, "B"));
        REQUIRE(p.add(c, "C"));
        ContextId target{};
        CHECK(p.authorize(a, 1, &target) == InputDecision::NotController); // nobody holds the lease
        CHECK(p.request_control(a));
        const auto epoch = p.lease().epoch;
        CHECK(p.authorize(a, 2, &target) == InputDecision::Accepted);
        CHECK_EQ(target.value, 7U);
        CHECK(p.authorize(b, 1, &target) == InputDecision::NotController);
        CHECK(!p.request_control(b));
        CHECK(!p.request_control(c));
        CHECK_EQ(p.lease().waiting.size(), 2U);
        p.release_control(a);
        CHECK(p.lease().holder == b);              // FIFO
        CHECK(p.lease().epoch > epoch);
        CHECK(p.authorize(a, 3, &target) == InputDecision::NotController); // former controller refused
        CHECK(p.authorize(b, 2, &target) == InputDecision::Accepted);
        // Replay of an already consumed sequence, and a non-increasing one.
        CHECK(p.authorize(b, 2, &target) == InputDecision::Replayed);
        CHECK(p.authorize(b, 1, &target) == InputDecision::Replayed);
        // Pause blocks the controller too.
        p.set_paused(true);
        CHECK(p.authorize(b, 3, &target) == InputDecision::Paused);
        p.set_paused(false);
        // Revoking the holder passes the lease on; the revoked id never comes back.
        p.remove(b, true);
        CHECK(p.lease().holder == c);
        CHECK(p.authorize(b, 10, &target) == InputDecision::Revoked);
        CHECK(!p.add(b, "B again"));
        CHECK(!p.host_grant(b));
        CHECK(p.host_grant(ParticipantId{}));      // host takes control back
        CHECK(p.authorize(c, 1, &target) == InputDecision::NotController);
    }
    // Mode A: each participant drives only its own context; a disconnection
    // keeps that context for the resume (regression: it used to be zeroed,
    // orphaning a live browser context and creating a second one).
    {
        SharePolicy p(ShareMode::Independent);
        p.set_shared_context(ContextId{1});
        REQUIRE(p.add(a, "A"));
        REQUIRE(p.add(b, "B"));
        ContextId target{};
        CHECK(p.authorize(a, 1, &target) == InputDecision::NoContext);
        p.set_context(a, ContextId{21});
        p.set_context(b, ContextId{22});
        CHECK(p.authorize(a, 2, &target) == InputDecision::Accepted);
        CHECK_EQ(target.value, 21U);
        CHECK(p.authorize(b, 1, &target) == InputDecision::Accepted);
        CHECK_EQ(target.value, 22U);
        CHECK(!p.request_control(a)); // no lease in mode A
        p.remove(a, false);
        REQUIRE(p.find(a));
        CHECK_EQ(p.find(a)->context.value, 21U);
        REQUIRE(p.add(a, "A resumed"));
        CHECK_EQ(p.find(a)->context.value, 21U);
        p.remove(a, true);
        CHECK(p.find(a) == nullptr);
    }
}

static void share_media_records_are_bounded() {
    using namespace plpt::share;
    // Hello roundtrip and refusal of any trailing/oversized bytes.
    auto hello = encode_hello(MediaHello{5, 9});
    RecordAssembler hello_reader(kMaxHelloRecord);
    REQUIRE(hello_reader.feed(hello));
    auto body = hello_reader.next();
    REQUIRE(body);
    auto decoded = decode_hello(*body);
    REQUIRE(decoded);
    CHECK_EQ(decoded.value().session, 5U);
    CHECK_EQ(decoded.value().participant, 9U);
    CHECK(!decode_hello(std::vector<std::uint8_t>{1, 0, 0}));
    // A length beyond the bound fails BEFORE any allocation and poisons the stream.
    RecordAssembler bounded(64);
    const std::uint8_t huge[] = {0x7F, 0xFF, 0xFF, 0xFF, 1};
    CHECK(!bounded.feed(huge));
    CHECK(bounded.failed());
    CHECK(!bounded.feed(hello));
    // Input record: zero sequence refused both ways, oversized text refused.
    InputRecord in;
    in.input_seq = 0;
    in.event.type = plpt::media::InputEventType::MouseMove;
    CHECK(!encode_input(in));
    in.input_seq = 42;
    in.event.x_norm = 0.25f;
    in.event.y_norm = 0.75f;
    auto rec = encode_input(in);
    REQUIRE(rec);
    RecordAssembler reader(kMaxControlRecord);
    REQUIRE(reader.feed(rec.value()));
    auto in_body = reader.next();
    REQUIRE(in_body);
    auto back = decode_input(*in_body);
    REQUIRE(back);
    CHECK_EQ(back.value().input_seq, 42U);
    auto zero = *in_body;
    for (int i = 1; i <= 8; ++i) zero[static_cast<std::size_t>(i)] = 0;
    CHECK(!decode_input(zero));
    // Control: only the two defined actions.
    auto ctl = encode_control(ControlAction::RequestControl);
    CHECK(decode_control(std::span<const std::uint8_t>(ctl).subspan(4)));
    const std::uint8_t bad_ctl[] = {static_cast<std::uint8_t>(RecordKind::Control), 9};
    CHECK(!decode_control(bad_ctl));
    // State roundtrip, invalid mode refused.
    StateRecord st;
    st.mode = ShareMode::Collaborative;
    st.you = 3;
    st.controller = 3;
    st.lease_epoch = 4;
    st.you_may_input = true;
    st.viewport_width = 1280;
    st.viewport_height = 720;
    st.note = "ok";
    auto st_rec = encode_state(st);
    auto st_back = decode_state(std::span<const std::uint8_t>(st_rec).subspan(4));
    REQUIRE(st_back);
    CHECK(st_back.value().you_may_input);
    CHECK(st_back.value().mode == ShareMode::Collaborative);
    auto bad_state = std::vector<std::uint8_t>(st_rec.begin() + 4, st_rec.end());
    bad_state[1] = 9;
    CHECK(!decode_state(bad_state));
    // The PSK identity is public and never contains the key.
    CHECK(media_identity(1, 2) == "devlink-media-s1-p2");
}

static void share_view_mapping_letterbox_and_text_split() {
    using namespace plpt::share;
    // 1280x720 frame in a 1000x1000 panel: full width, centred vertically.
    const auto r = fit_letterbox(ViewRect{100, 50, 1000, 1000}, 1280, 720);
    CHECK_EQ(r.width, 1000.0f);
    CHECK_EQ(r.height, 562.0f);
    CHECK_EQ(r.x, 100.0f);
    CHECK_EQ(r.y, 50.0f + 219.0f);
    // Centre of the picture -> (0.5, 0.5) whatever the panel or DPI.
    auto centre = to_normalized(r, r.x + r.width / 2, r.y + r.height / 2, false);
    REQUIRE(centre);
    CHECK(std::fabs(centre->x - 0.5f) < 1e-4f);
    CHECK(std::fabs(centre->y - 0.5f) < 1e-4f);
    // The same logical point at 200 % (all coordinates doubled) maps identically.
    const auto r2 = fit_letterbox(ViewRect{200, 100, 2000, 2000}, 1280, 720);
    auto centre2 = to_normalized(r2, r2.x + r2.width / 2, r2.y + r2.height / 2, false);
    REQUIRE(centre2);
    CHECK(std::fabs(centre2->x - centre->x) < 1e-3f);
    // Letterbox bars are not part of the page; a drag is clamped to the edge.
    CHECK(!to_normalized(r, 500, 60, false));
    auto clamped = to_normalized(r, 500, 60, true);
    REQUIRE(clamped);
    CHECK_EQ(clamped->y, 0.0f);
    CHECK(fit_letterbox(ViewRect{0, 0, 100, 100}, 0, 720).width == 0.0f);
    CHECK(!to_normalized(ViewRect{}, 1, 1, true));
    // UTF-8 split never cuts a code point and drops invalid lead bytes.
    std::vector<std::string> parts;
    split_utf8("é✓abc", 4, [&](const char* p, std::size_t n) { parts.emplace_back(p, n); });
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "é");
    CHECK(parts[1] == "✓a");
    CHECK(parts[2] == "bc");
    parts.clear();
    split_utf8("Bob écrit ✓ — 32 octets au plus par enregistrement", plpt::media::kMaxInputTextBytes,
        [&](const char* p, std::size_t n) { parts.emplace_back(p, n); });
    std::string joined;
    for (const auto& part : parts) {
        CHECK(part.size() <= plpt::media::kMaxInputTextBytes);
        joined += part;
    }
    CHECK(joined == "Bob écrit ✓ — 32 octets au plus par enregistrement");
    parts.clear();
    const char invalid[] = {'a', static_cast<char>(0xFF), 'b', 0};
    split_utf8(invalid, 32, [&](const char* p, std::size_t n) { parts.emplace_back(p, n); });
    std::string kept;
    for (const auto& part : parts) kept += part;
    CHECK(kept == "ab");
}

int main() {
    CASE(result_invariants); CASE(exact_classifier_and_ancestry); CASE(cycles_missing_and_reused_pid);
    CASE(registry_grouping_and_no_auto_share); CASE(registry_churn_and_incomplete); CASE(unknown_owner_cannot_retain_selection);
    CASE(state_expiry_and_cleanup); CASE(closing_revokes_without_waiting_for_expiry); CASE(roles_contexts_control_and_revocation);
    CASE(five_model_guests_and_capacity); CASE(messages_dm_privacy_and_read_receipts); CASE(messages_quotas_and_feedback_redaction);
    CASE(text_validation); CASE(protocol_golden_and_bounds);
    CASE(hmac_sha256_matches_rfc4231); CASE(challenge_response_codec_and_derivation); CASE(protocol_deterministic_mutations); CASE(estimates_are_bounded);
    CASE(audit_is_opt_in_typed_and_bounded); CASE(demo_browser_isolation_and_allowlist); CASE(unavailable_backends_fail_closed);
    CASE(observation_brackets_process_identity); CASE(observation_keeps_first_snapshot_warning);
    CASE(partial_registry_history_is_bounded); CASE(partial_process_inventory_invalidates_selection);
    CASE(registry_capacity_failure_invalidates_selection); CASE(worker_publishes_immutable_snapshots);
    CASE(worker_invalidates_selection_on_partial_or_error); CASE(worker_command_queue_is_bounded);
    CASE(wire_messages_roundtrip_and_guard); CASE(secrets_digests_and_formats_are_exact);
    CASE(proxy_handshake_is_bounded_and_exact); CASE(discovery_announcement_codec_is_bounded);
    CASE(capability_is_never_forwarded_upstream); CASE(bind_resolution_never_defaults_to_any);
    CASE(resume_secret_is_the_only_proof_of_a_place);
    CASE(remote_frame_and_input_serialization); CASE(bounded_frame_queue_enforces_limits_and_lifecycle);
    CASE(browser_backend_selection_and_lifecycle);
    CASE(share_policy_modes_are_enforced_server_side); CASE(share_media_records_are_bounded);
    CASE(share_view_mapping_letterbox_and_text_split);
    return plpt_test::finish("unit");
}

