#include <array>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <string_view>
#ifdef _WIN32
#include "engine/platform/windows/winsock.hpp"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "tests/test_harness.hpp"
#include "engine/core/random.hpp"
#include <atomic>
#include <mutex>
#include "engine/platform/connection_pool.hpp"
#include "engine/platform/native.hpp"
#include "engine/discovery/monitor.hpp"
#include "engine/port_allocator/port_allocator.hpp"
#include <chrono>
#include <thread>
#include "engine/core/clock.hpp"
#include "engine/core/secrets.hpp"
#include "engine/net/discovery.hpp"
#include "engine/net/guest_client.hpp"
#include "engine/net/host_server.hpp"
#include "engine/net/proxy.hpp"

using namespace plpt;
namespace {
ProcessId self() {
#ifdef _WIN32
    return ProcessId{GetCurrentProcessId()};
#else
    return ProcessId{static_cast<std::uint32_t>(getpid())};
#endif
}
bool try_bind(Port port, IpFamily family, TransportProtocol protocol) {
    const auto fam = family == IpFamily::Ipv4 ? AF_INET : AF_INET6;
    const auto type = protocol == TransportProtocol::Tcp ? SOCK_STREAM : SOCK_DGRAM;
    const auto handle = ::socket(fam, type, 0);
#ifdef _WIN32
    if (handle == INVALID_SOCKET) return false;
#else
    if (handle < 0) return false;
#endif
    sockaddr_storage addr{};
    int status;
    if (fam == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&addr);
        a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); a->sin_port = htons(port.value);
        status = ::bind(handle, reinterpret_cast<sockaddr*>(a), sizeof(*a));
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
        a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback; a->sin6_port = htons(port.value);
        status = ::bind(handle, reinterpret_cast<sockaddr*>(a), sizeof(*a));
    }
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
    return status == 0;
}
void entropy_os() {
    SecureRandom random;
    std::array<std::uint8_t, 32> data{};
    REQUIRE(random.fill(data));
    CHECK(random.fill({}));
    // Pas de « test statistique » pour prétendre prouver un CSPRNG.
}
void allocation_exclusion_move_and_release(IpFamily family) {
    DynamicPortAllocator allocator;
    for (const auto protocol : {TransportProtocol::Tcp, TransportProtocol::Udp}) {
        Port released;
        {
            auto reserved = allocator.acquire(protocol, family); REQUIRE(reserved);
            auto other = allocator.acquire(protocol, family); REQUIRE(other);
            REQUIRE(reserved.value().valid()); CHECK(reserved.value().port().value != 0);
            CHECK(reserved.value().port() != other.value().port());
            released = reserved.value().port();
            CHECK(!try_bind(released, family, protocol));
            ReservedListener moved = std::move(reserved).value();
            CHECK(!reserved.value().valid()); CHECK_EQ(reserved.value().port(), Port{});
            CHECK(moved.valid()); CHECK(!try_bind(released, family, protocol));
            auto& same = moved;
            moved = std::move(same); // auto-affectation volontaire, RAII doit rester valide.
            CHECK(moved.valid());
        }
        CHECK(try_bind(released, family, protocol)); // réservation réellement libérée, aucun check-then-bind produit.
    }
    CHECK(!allocator.acquire(TransportProtocol::Unknown, family));
    CHECK(!allocator.acquire(TransportProtocol::Udp, IpFamily::Unknown));
}
void ipv4_allocation() { allocation_exclusion_move_and_release(IpFamily::Ipv4); }
void ipv6_allocation() { allocation_exclusion_move_and_release(IpFamily::Ipv6); }
void live_discovery(IpFamily family) {
    DynamicPortAllocator allocator;
    auto tcp = allocator.acquire(TransportProtocol::Tcp, family); REQUIRE(tcp);
    auto udp = allocator.acquire(TransportProtocol::Udp, family); REQUIRE(udp);
    NativeSocketTableProvider sockets;
    NativeProcessEnumerator processes;
    auto captured = observe(processes, sockets); REQUIRE(captured);
    CHECK(captured.value().sockets.complete);
    bool saw_tcp = false, saw_udp = false, saw_self = false;
    for (const auto& r : captured.value().sockets.records) {
        if (r.local.family != family) continue;
        if (r.protocol == TransportProtocol::Tcp && r.local.port == tcp.value().port() && r.local.address == tcp.value().endpoint().address) {
            CHECK_EQ(r.state, SocketState::Listening); CHECK_EQ(r.owner_pid, std::optional{self()}); saw_tcp = true;
        }
        if (r.protocol == TransportProtocol::Udp && r.local.port == udp.value().port() && r.local.address == udp.value().endpoint().address) {
            CHECK_EQ(r.state, SocketState::Bound); CHECK_EQ(r.owner_pid, std::optional{self()}); saw_udp = true;
        }
    }
    for (const auto& p : captured.value().processes.records) {
        if (p.pid != self()) continue;
        saw_self = true;
        CHECK(p.ppid.value != 0); CHECK(!p.image_name.empty());
        CHECK(p.start_time_ticks != 0); CHECK(p.executable_path);
    }
    CHECK(saw_tcp); CHECK(saw_udp); CHECK(saw_self);
}
void ipv4_native_tables_and_process() { live_discovery(IpFamily::Ipv4); }
void ipv6_native_tables_and_process() { live_discovery(IpFamily::Ipv6); }
void interface_addresses() {
    NativeNetworkInterfaceProvider provider;
    auto result = provider.enumerate(); REQUIRE(result);
    CHECK(!result.value().empty());
    bool loopback = false;
    for (const auto& iface : result.value()) {
        CHECK(!iface.address.empty());
        CHECK(iface.family == IpFamily::Ipv4 || iface.family == IpFamily::Ipv6);
        loopback = loopback || iface.is_loopback;
    }
    CHECK(loopback);
}
#ifndef _WIN32
namespace fs = std::filesystem;
struct Fixture {
    fs::path path;
    Fixture() {
        SecureRandom rng;
        std::array<std::uint8_t, 12> bytes{};
        if (!rng.fill(bytes)) throw std::runtime_error("fixture RNG failed");
        std::string name = "plpt-test-";
        for (auto b : bytes) name += std::to_string(b) + "-";
        path = fs::temp_directory_path() / name;
        if (!fs::create_directory(path)) throw std::runtime_error("fixture create failed");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(path, ec); } // uniquement CE fixture possédé par le test.
    void file(const fs::path& relative, const std::string& text) {
        fs::create_directories((path / relative).parent_path());
        std::ofstream stream(path / relative); stream << text;
        if (!stream) throw std::runtime_error("fixture write failed");
    }
};
std::string row(std::string address = "0100007F:ABCD", std::string state = "0A") {
    const auto remote = address.size() == 13 ? "00000000:0000" : "00000000000000000000000000000000:0000";
    return "0: " + address + " " + remote + " " + state + " 00000000:00000000 00:00000000 00000000 1000 0 777 1 0 0 0\n";
}
void proc_parsers_and_fixtures() {
    auto v4 = parse_proc_socket(row(), TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(v4);
    CHECK_EQ(v4.value().socket.local.address, "127.0.0.1"); CHECK_EQ(v4.value().socket.local.port.value, 0xABCD);
    CHECK_EQ(v4.value().inode, 777U);
    auto v6 = parse_proc_socket(row("00000000000000000000000001000000:ABCD"), TransportProtocol::Tcp, IpFamily::Ipv6); REQUIRE(v6);
    CHECK_EQ(v6.value().socket.local.address, "::1");
    CHECK(!parse_proc_socket("broken", TransportProtocol::Tcp, IpFamily::Ipv4));
    CHECK(!parse_proc_socket(row("GG00007F:ABCD"), TransportProtocol::Tcp, IpFamily::Ipv4));
    CHECK(!parse_proc_socket(row("0100007F:10000"), TransportProtocol::Tcp, IpFamily::Ipv4));
    auto fin = parse_proc_socket(row("0100007F:ABCD", "05"), TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(fin);
    CHECK(!is_service_endpoint(fin.value().socket));
    std::string stat = "123 (name with ) space) S 10";
    for (int field = 5; field <= 21; ++field) stat += " 0";
    stat += " 987654 999999999 0\n";
    auto process = parse_proc_stat(stat); REQUIRE(process);
    CHECK_EQ(process.value().image_name, "name with ) space"); CHECK_EQ(process.value().ppid, ProcessId{10});
    CHECK_EQ(process.value().start_time_ticks, 987654U); // champ 22, PAS vsize champ 23.
    CHECK(!parse_proc_stat("123 no parenthesis"));
    CHECK(!parse_proc_stat("99999999999999999999 (x) S 0"));
    Fixture fixture;
    fixture.file("net/tcp", "header\n" + row());
    for (const auto name : {"tcp6", "udp", "udp6"}) fixture.file(fs::path("net") / name, "header\n");
    fixture.file("123/stat", stat);
    fs::create_directories(fixture.path / "123/fd");
    fs::create_symlink("socket:[777]", fixture.path / "123/fd/3");
    LinuxSocketTableProvider provider(fixture.path.string());
    auto sockets = provider.snapshot(); REQUIRE(sockets); REQUIRE(sockets.value().records.size() == 1);
    CHECK_EQ(sockets.value().records[0].owner_pid, std::optional{ProcessId{123}});
    fs::create_directories(fixture.path / "124/fd");
    fs::create_symlink("socket:[777]", fixture.path / "124/fd/4");
    auto ambiguous = provider.snapshot(); REQUIRE(ambiguous); CHECK(!ambiguous.value().records[0].owner_pid);
    fs::remove(fixture.path / "net/tcp6");
    auto incomplete = provider.snapshot(); REQUIRE(incomplete); CHECK(!incomplete.value().complete); CHECK(!incomplete.value().warnings.empty());
    LinuxProcessEnumerator enumerator(fixture.path.string());
    auto processes = enumerator.enumerate(); REQUIRE(processes); REQUIRE(processes.value().records.size() == 1);
    CHECK_EQ(processes.value().records[0].start_time_ticks, 987654U);
    LinuxSocketTableProvider nonexistent((fixture.path / "absent").string()); CHECK(!nonexistent.snapshot());
}
std::size_t fd_count() { return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{})); }
void repeated_allocation_releases_fds() {
    const auto before = fd_count();
    DynamicPortAllocator allocator;
    for (int i = 0; i < 100; ++i) {
        auto one = allocator.acquire(TransportProtocol::Udp, IpFamily::Ipv4); REQUIRE(one);
        auto two = allocator.acquire(TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(two);
        one.value() = std::move(two).value();
    }
    CHECK_EQ(fd_count(), before);
}
#endif

// ---- Réseau LAN réel : plan de contrôle, plan de données, découverte. ----
// Tous les ports sont éphémères et toutes les adresses en loopback : ces cas sont
// exécutables en CI sans aucun privilège ni configuration réseau particulière.
using namespace std::chrono_literals;
using namespace plpt::net;

namespace {
// Amont minimal : répond une fois par connexion, puis ferme. Sert à qualifier le
// proxy sans dépendre d'un binaire externe.
class TinyUpstream {
public:
    bool start(std::string body) { return start(std::move(body), std::string(), std::string()); }
    // `prelude` : octets envoyés AVANT la réponse finale (100 Continue, 103 Early
    // Hints). `raw` : réponse envoyée telle quelle, sans réponse finale (101, réponse
    // illisible). `last_request()` expose ce que l'amont a réellement reçu.
    bool start(std::string body, std::string prelude, std::string raw) {
        body_ = std::move(body);
        prelude_ = std::move(prelude);
        raw_ = std::move(raw);
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
        if (!created) return false;
        listener_ = std::move(created).value();
        if (!listener_.bind(IpAddress::loopback(IpFamily::Ipv4), Port{0})) return false;
        auto bound = listener_.bound_port();
        if (!bound) return false;
        port_ = bound.value().value;
        if (!listener_.listen(8)) return false;
        // Non bloquant : un accept() bloquant ne se réveille pas de façon fiable
        // quand la socket est fermée depuis un autre thread (arrêt impossible).
        if (!listener_.set_nonblocking(true)) return false;
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
        return true;
    }
    void stop() {
        if (worker_.joinable()) { worker_.request_stop(); }
        listener_.close();
        if (worker_.joinable()) worker_.join();
    }
    std::uint16_t port() const { return port_; }
    unsigned served() const { return served_; }
    std::string last_request() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return last_request_;
    }
private:
    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            IpAddress peer; Port peer_port{};
            auto accepted = listener_.accept(&peer, &peer_port);
            if (!accepted) {
                if (accepted.error().code == ErrorCode::WouldBlock) { std::this_thread::sleep_for(20ms); continue; }
                return;
            }
            auto socket = std::move(accepted).value();
            std::vector<std::uint8_t> buffer(4096);
            std::string request;
            const auto deadline = std::chrono::steady_clock::now() + 3000ms;
            while (std::chrono::steady_clock::now() < deadline && request.find("\r\n\r\n") == std::string::npos) {
                auto received = socket.receive(buffer);
                if (!received || received.value() == 0) break;
                request.append(reinterpret_cast<const char*>(buffer.data()), received.value());
            }
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                last_request_ = request;
            }
            if (!raw_.empty()) {
                // Réponse imposée (101, illisible) : envoyée telle quelle, puis on
                // reste jusqu'à la fermeture du client ou un délai court.
                (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(raw_.data()), raw_.size()}, 2000ms);
                (void)socket.set_timeouts(400ms, 400ms);
                std::vector<std::uint8_t> drain(1024);
                for (int round = 0; round < 3; ++round) {
                    auto received = socket.receive(drain);
                    if (!received || received.value() == 0) break;
                }
                socket.close();
                ++served_;
                continue;
            }
            if (!prelude_.empty())
                (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(prelude_.data()), prelude_.size()}, 2000ms);
            // Tout l'en-tête reçu est renvoyé dans le corps : c'est la preuve de ce
            // que le service local voit réellement, après retrait de la capacité PLPT.
            const auto head_end = request.find("\r\n\r\n");
            const std::string head = request.substr(0, head_end == std::string::npos ? request.size() : head_end);
            const std::string payload = body_ + "\n" + head;
            const auto response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
            socket.close();
            ++served_;
        }
    }
    Socket listener_;
    std::jthread worker_;
    std::string body_;
    std::string prelude_;
    std::string raw_;
    std::string last_request_;
    mutable std::mutex mutex_;
    std::uint16_t port_ = 0;
    unsigned served_ = 0;
};
} // namespace

void lan_control_plane_host_and_guest() {
    SystemClock clock;
    auto generated = generate_invite_code(); REQUIRE(generated);
    const InviteCode code = generated.value();
    HostConfig config;
    config.session_name = "Session intégration";
    config.host_name = "hôte-test";
    config.bind_address = IpAddress::loopback(IpFamily::Ipv4);
    config.max_participants = 2;
    config.approval_required = true;
    config.heartbeat_interval_ms = 200;
    config.inactivity_timeout_ms = 5000;
    config.auth_timeout_ms = 3000;
    config.approval_timeout_ms = 5000;
    config.invite_code = code;
    HostServer host(clock);
    auto started = host.start(config); REQUIRE(started);
    CHECK(started.value() != 0);

    // Code erroné : refus explicite, jamais d'admission silencieuse.
    {
        GuestConfig wrong;
        wrong.address = IpAddress::loopback(IpFamily::Ipv4);
        wrong.control_port = started.value();
        wrong.guest_name = "intrus";
        wrong.invite_code = generate_invite_code().value();
        GuestClient intruder(clock);
        REQUIRE(intruder.start(wrong));
        CHECK(intruder.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::Refused; }, 5s));
        CHECK_EQ(std::string(guest_phase_text(intruder.snapshot()->phase)), std::string("refusé"));
        intruder.stop();
    }

    // Code valide : attente d'autorisation, puis admission sur décision de l'hôte.
    GuestConfig alice_config;
    alice_config.address = IpAddress::loopback(IpFamily::Ipv4);
    alice_config.control_port = started.value();
    alice_config.guest_name = "Alice";
    alice_config.invite_code = code;
    alice_config.heartbeat_interval_ms = 200;
    GuestClient alice(clock);
    REQUIRE(alice.start(alice_config));
    REQUIRE(alice.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::AwaitingApproval; }, 5s));
    REQUIRE(host.wait_for([](const HostSnapshot& snapshot) { return snapshot.pending.size() == 1; }, 5s));
    {
        const auto view = host.snapshot();
        CHECK_EQ(view->pending[0].guest_name, std::string("Alice"));
        CHECK_EQ(view->invite_code, code.text);   // affiché à l'hôte, jamais annoncé sur le LAN.
        // Version lisible dérivée des constantes : l'attente ne vieillit pas toute seule.
        CHECK_EQ(view->pending[0].client, std::string("PLPT ") + std::to_string(plpt::model::kProtocolMajor) + "." +
            std::to_string(plpt::model::kProtocolMinor));
    }
    // Services publiés : port exposé et URL à jeton, pas le port local.
    auto token = generate_service_token(); REQUIRE(token);
    {
        std::vector<SharedServiceRecord> services;
        SharedServiceRecord record;
        record.key = 1; record.label = "Démo HTTP"; record.local_port = 43210; record.exposed_port = 43211;
        record.token = token.value();
        record.url = "http://127.0.0.1:43211/?plpt=" + token.value();
        services.push_back(record);
        CHECK(host.set_services(services));
    }
    CHECK(host.approve(host.snapshot()->pending[0].request_id));
    REQUIRE(alice.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::Connected; }, 5s));
    REQUIRE(alice.wait_for([](const GuestSnapshot& snapshot) { return !snapshot.services.empty(); }, 5s));
    {
        const auto view = alice.snapshot();
        CHECK(view->self.value != 0);
        CHECK_EQ(view->session_name, std::string("Session intégration"));
        CHECK_EQ(view->host_name, std::string("hôte-test"));
        CHECK_EQ(view->services[0].exposed_port, 43211);
        CHECK(view->services[0].url.find("plpt=") != std::string::npos);
        CHECK(view->services[0].url.find(token.value()) != std::string::npos);
    }
    CHECK(host.wait_for([](const HostSnapshot& snapshot) { return snapshot.participants.size() == 1; }, 5s));

    // Second invité, messagerie générale et privée relayées par l'autorité hôte.
    GuestConfig bob_config = alice_config; bob_config.guest_name = "Bob";
    GuestClient bob(clock);
    REQUIRE(bob.start(bob_config));
    REQUIRE(bob.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::AwaitingApproval; }, 5s));
    CHECK(host.approve(host.snapshot()->pending[0].request_id));
    REQUIRE(bob.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::Connected; }, 5s));
    CHECK(host.wait_for([](const HostSnapshot& snapshot) { return snapshot.participants.size() == 2; }, 5s));
    CHECK(host.post_chat("Bonjour depuis l'hôte"));
    CHECK(alice.wait_for([](const GuestSnapshot& snapshot) {
        for (const auto& line : snapshot.chat) if (line.body == "Bonjour depuis l'hôte") return true;
        return false;
    }, 5s));
    const auto alice_id = alice.snapshot()->self;
    CHECK(bob.post_chat("Message de Bob"));
    CHECK(alice.wait_for([](const GuestSnapshot& snapshot) {
        for (const auto& line : snapshot.chat) if (line.body == "Message de Bob" && line.sender_name == "Bob") return true;
        return false;
    }, 5s));
    CHECK(bob.post_direct_message(alice_id, "Privé pour Alice"));
    CHECK(alice.wait_for([](const GuestSnapshot& snapshot) {
        for (const auto& line : snapshot.chat) if (line.body == "Privé pour Alice" && line.direct) return true;
        return false;
    }, 5s));
    // Un message privé n'est jamais rediffusé sur le canal général.
    CHECK(!bob.wait_for([](const GuestSnapshot& snapshot) {
        for (const auto& line : snapshot.chat) if (line.body == "Privé pour Alice" && !line.direct) return true;
        return false;
    }, 400ms));
    CHECK(!bob.post_direct_message(ParticipantId{4242}, "inconnu")); // pair absent de la liste publiée.
    CHECK(!bob.post_direct_message(bob.snapshot()->self, "soi-même"));
    // Battements : la latence mesurée est un aller-retour réel, jamais une estimation.
    CHECK(alice.wait_for([](const GuestSnapshot& snapshot) { return snapshot.counters.frames_received >= 4; }, 5s));

    // Capacité atteinte : l'admission est refusée et le motif traverse le réseau.
    GuestConfig third_config = alice_config; third_config.guest_name = "Trop";
    GuestClient third(clock);
    REQUIRE(third.start(third_config));
    REQUIRE(third.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::AwaitingApproval; }, 5s));
    CHECK(host.approve(host.snapshot()->pending[0].request_id));
    CHECK(third.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::Refused; }, 5s));
    CHECK_EQ(third.snapshot()->status_text, std::string("capacité atteinte"));

    // Révocation : l'invité perd l'accès immédiatement, l'hôte voit l'état.
    CHECK(host.revoke(alice_id));
    CHECK(alice.wait_for([](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::Disconnected; }, 5s));
    CHECK(host.wait_for([&](const HostSnapshot& snapshot) {
        for (const auto& participant : snapshot.participants)
            if (participant.id == alice_id) return participant.state == plpt::model::ParticipantState::Revoked;
        return false;
    }, 5s));
    CHECK(!alice.post_chat("après révocation"));

    // Journal et compteurs réels ; aucun secret en clair.
    {
        const auto view = host.snapshot();
        CHECK(view->counters.admitted >= 2);
        CHECK(view->counters.bad_codes >= 1);
        CHECK(view->counters.denied >= 1);
        CHECK(view->counters.revoked == 1);
        CHECK(view->counters.chat_relayed >= 3);
        CHECK(!view->events.empty());
        for (const auto& event : view->events) {
            CHECK(event.find(code.text) == std::string::npos);
            CHECK(event.find(token.value()) == std::string::npos);
        }
    }
    third.stop(); bob.stop(); alice.stop();
    host.stop();
    CHECK(!host.running());
    const auto final_view = host.snapshot();
    CHECK(final_view->state == plpt::model::SessionState::Closed);
    CHECK_EQ(final_view->connection_count, 0U);
}

// Échange HTTP complet sur la boucle locale : renvoie tout ce que le client reçoit.
std::string http_exchange(std::uint16_t port, const std::string& target, const std::string& extra_headers,
    std::chrono::milliseconds budget = std::chrono::milliseconds(6000)) {
    auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
    if (!created) return {};
    auto socket = std::move(created).value();
    if (!connect_with_timeout(socket, IpAddress::loopback(IpFamily::Ipv4), Port{port}, std::chrono::milliseconds(3000)))
        return {};
    const std::string text = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n" + extra_headers + "\r\n";
    if (!write_all(socket, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, std::chrono::milliseconds(3000)))
        return {};
    std::string raw;
    std::vector<std::uint8_t> buffer(8192);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        auto received = socket.receive(buffer);
        if (!received || received.value() == 0) break;
        raw.append(reinterpret_cast<const char*>(buffer.data()), received.value());
    }
    socket.close();
    return raw;
}

ProxyConfig loopback_proxy_for(const TinyUpstream& upstream, const std::string& token) {
    ProxyConfig config;
    config.label = "Démo";
    config.bind_address = IpAddress::loopback(IpFamily::Ipv4);
    config.upstream_address = IpAddress::loopback(IpFamily::Ipv4);
    config.upstream_port = upstream.port();
    config.token = token;
    config.handshake_timeout_ms = 3000;
    config.idle_timeout_ms = 20000;
    return config;
}

void lan_data_plane_proxy_token_gate() {
    SystemClock clock;
    TinyUpstream upstream;
    REQUIRE(upstream.start("contenu réel du service local"));
    auto token = generate_service_token(); REQUIRE(token);
    ProxyConfig config;
    config.label = "Démo HTTP";
    config.bind_address = IpAddress::loopback(IpFamily::Ipv4);
    config.upstream_address = IpAddress::loopback(IpFamily::Ipv4);
    config.upstream_port = upstream.port();
    config.token = token.value();
    config.handshake_timeout_ms = 3000;
    config.idle_timeout_ms = 20000;
    ServiceProxy proxy(clock);
    auto started = proxy.start(config); REQUIRE(started);
    CHECK(proxy.probe_upstream());
    REQUIRE(proxy.wait_for([](const ProxySnapshot& snapshot) { return snapshot.upstream_alive; }, 5s));
    CHECK_EQ(proxy.snapshot()->upstream, "127.0.0.1:" + std::to_string(upstream.port()));
    CHECK(proxy.snapshot()->url.find(token.value()) != std::string::npos);

    // Même client HTTP que le cas des réponses informationnelles : une seule
    // implémentation d'échange pour tout le plan de données.
    const auto request = [&](const std::string& target, const std::string& headers) {
        return http_exchange(started.value(), target, headers, 4s);
    };
    // Corps renvoyé par l'amont = ce que le service local a réellement reçu.
    const auto upstream_view = [](const std::string& response) {
        const auto separator = response.find("\r\n\r\n");
        return separator == std::string::npos ? std::string() : response.substr(separator + 4);
    };
    {   // Le jeton d'accès ne traverse jamais le proxy : ni URL, ni journal amont.
        const auto response = request("/?plpt=" + token.value(), "");
        CHECK(response.find("HTTP/1.1 200") == 0);
        CHECK(response.find("contenu réel du service local") != std::string::npos);
        const auto seen = upstream_view(response);
        CHECK(seen.find("GET / HTTP/1.1") != std::string::npos); // cible réécrite, jeton retiré
        CHECK(seen.find(token.value()) == std::string::npos);
        CHECK(seen.find("plpt") == std::string::npos);
        // La capacité est rendue à l'invité sous forme de cookie, pour les sous-ressources.
        CHECK(response.find("Set-Cookie: plpt=" + token.value()) != std::string::npos);
        CHECK(response.find("HttpOnly") != std::string::npos);
        CHECK(response.find("SameSite=Strict") != std::string::npos);
        CHECK(response.find("Max-Age") == std::string::npos);
    }
    {   // Autres paramètres conservés, seul le paramètre de capacité disparaît.
        const auto response = request("/app?a=1&plpt=" + token.value() + "&b=2", "");
        CHECK(response.find("HTTP/1.1 200") == 0);
        CHECK(upstream_view(response).find("GET /app?a=1&b=2 HTTP/1.1") != std::string::npos);
        CHECK(upstream_view(response).find(token.value()) == std::string::npos);
    }
    {
        const auto response = request("/", "");
        CHECK(response.find("HTTP/1.1 403") == 0);
        CHECK(response.find("Content-Length: ") != std::string::npos);
    }
    {
        const auto response = request("/?plpt=00000000000000000000000000000000", "");
        CHECK(response.find("HTTP/1.1 403") == 0);
    }
    {   // En-tête dédié retiré, Referer nettoyé : aucune fuite vers le service local.
        const auto response = request("/", "X-PLPT-Token: " + token.value() + "\r\nReferer: http://hote/?plpt=" + token.value() + "\r\n");
        CHECK(response.find("HTTP/1.1 200") == 0);
        const auto seen = upstream_view(response);
        CHECK(seen.find("X-PLPT-Token") == std::string::npos);
        CHECK(seen.find("Referer: http://hote/") != std::string::npos);
        CHECK(seen.find(token.value()) == std::string::npos);
    }
    {   // Capacité présentée par cookie : autorisée, retirée, et non re-déposée.
        const auto response = request("/", "Cookie: theme=dark; plpt=" + token.value() + "\r\n");
        CHECK(response.find("HTTP/1.1 200") == 0);
        const auto seen = upstream_view(response);
        CHECK(seen.find("Cookie: theme=dark") != std::string::npos); // les autres cookies passent
        CHECK(seen.find(token.value()) == std::string::npos);
        CHECK(response.find("Set-Cookie:") == std::string::npos);   // déjà un cookie : rien à déposer
    }
    // Requête sans fin d'en-têtes : le proxy répond et ferme, il n'attend pas indéfiniment.
    {
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(created);
        auto socket = std::move(created).value();
        REQUIRE(connect_with_timeout(socket, IpAddress::loopback(IpFamily::Ipv4), Port{started.value()}, 3s));
        const std::string partial = "GET / HTTP/1.1\r\nHost: x\r\n";
        REQUIRE(write_all(socket, {reinterpret_cast<const std::uint8_t*>(partial.data()), partial.size()}, 3s));
        socket.close();
        CHECK(proxy.wait_for([](const ProxySnapshot& snapshot) { return snapshot.counters.connections_total >= 6; }, 5s));
    }
    {
        const auto view = proxy.snapshot();
        CHECK(view->counters.authorized >= 4);
        CHECK(view->counters.refused >= 2);
        CHECK(view->counters.tokens_redacted >= 4);  // chaque requête autorisée est nettoyée
        CHECK(view->counters.grants_issued >= 3);    // URL et en-tête dédié, pas le cookie
        CHECK_EQ(view->counters.grants_failed, 0U);
        CHECK(!view->any_interface);                 // boucle locale imposée par le test
        CHECK(view->bind_address == "127.0.0.1");
        CHECK(!view->bind_note.empty());
        CHECK(view->counters.bytes_to_upstream > 0);
        CHECK(view->counters.bytes_to_guest > 0);
        for (const auto& event : view->events) CHECK(event.find(token.value()) == std::string::npos);
    }
    proxy.stop();
    CHECK(!proxy.running());

    // Amont injoignable : 502 explicite, jamais un silence ni un faux succès.
    {
        ProxyConfig dead = config;
        dead.label = "Service éteint";
        dead.upstream_port = 1;   // port fermé : connexion refusée garantie.
        ServiceProxy dead_proxy(clock);
        auto dead_started = dead_proxy.start(dead); REQUIRE(dead_started);
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(created);
        auto socket = std::move(created).value();
        REQUIRE(connect_with_timeout(socket, IpAddress::loopback(IpFamily::Ipv4), Port{dead_started.value()}, 3s));
        const std::string text = "GET /?plpt=" + token.value() + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        REQUIRE(write_all(socket, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, 3s));
        std::string raw; std::vector<std::uint8_t> buffer(4096);
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto received = socket.receive(buffer);
            if (!received || received.value() == 0) break;
            raw.append(reinterpret_cast<const char*>(buffer.data()), received.value());
        }
        socket.close();
        CHECK(raw.find("HTTP/1.1 502") == 0);
        CHECK(dead_proxy.wait_for([](const ProxySnapshot& snapshot) {
            return !snapshot.upstream_alive && snapshot.counters.upstream_failures >= 1;
        }, 5s));
        dead_proxy.stop();
    }
    upstream.stop();
}

void lan_discovery_announce_and_listen() {
    SystemClock clock;
    // Écoute en loopback sur un port éphémère : ni multicast ni port de production,
    // donc exécutable en CI sans privilège ni configuration réseau.
    DiscoveryListener::Config listen_config;
    listen_config.bind = IpAddress::loopback(IpFamily::Ipv4);
    listen_config.port = Port{0};
    listen_config.join_multicast = false;
    listen_config.allow_broadcast = false;
    DiscoveryListener listener(clock);
    REQUIRE(listener.start(listen_config));
    const auto bound = listener.bound_port();
    REQUIRE(bound.value != 0);

    Announcement announcement;
    announcement.session_id = 987654;
    announcement.session_name = "Session découverte";
    announcement.host_name = "hôte-test";
    announcement.control_port = 40000;
    announcement.participants = 2;
    announcement.max_participants = 5;
    announcement.shared_services = 1;
    announcement.accepting = true;
    announcement.sequence = 1;
    announcement.wall_ms = clock.now_wall_ms();

    Announcer::Config announce_config;
    announce_config.announcement = announcement;
    announce_config.source = IpAddress::loopback(IpFamily::Ipv4);
    announce_config.extra_targets.push_back({IpAddress::loopback(IpFamily::Ipv4), bound});
    announce_config.interval_ms = 250;
    Announcer announcer(clock);
    REQUIRE(announcer.start(announce_config));
    REQUIRE(announcer.send_now());
    CHECK(listener.wait_for_sessions(1, 5s));
    auto view = listener.snapshot();
    REQUIRE(view);
    REQUIRE(!view->empty());
    const auto& session = (*view)[0];
    CHECK_EQ(session.session_name, std::string("Session découverte"));
    CHECK_EQ(session.host_name, std::string("hôte-test"));
    CHECK_EQ(session.control_port, 40000);
    CHECK_EQ(session.participants, 2);
    CHECK_EQ(session.max_participants, 5);
    CHECK_EQ(session.shared_services, 1);
    CHECK(session.accepting);
    CHECK_EQ(session.address.text, std::string("127.0.0.1")); // adresse OBSERVÉE, jamais celle annoncée.
    CHECK_EQ(session.session_id, 987654U);
    CHECK(!session.manual);
    CHECK(listener.accepted_datagrams() >= 1);

    // Déduplication : une seconde annonce de la même session ne crée pas d'entrée.
    REQUIRE(announcer.send_now());
    std::this_thread::sleep_for(500ms);
    view = listener.snapshot();
    REQUIRE(view);
    CHECK_EQ(view->size(), 1U);
    announcer.stop();

    // Datagrammes illisibles : comptés comme rejetés, l'écoute continue.
    {
        auto junk = Socket::create(TransportProtocol::Udp, IpFamily::Ipv4);
        REQUIRE(junk);
        auto sender = std::move(junk).value();
        const std::string garbage = "PAS-UNE-ANNONCE-PLPT";
        for (int index = 0; index < 3; ++index)
            (void)sender.send_to(IpAddress::loopback(IpFamily::Ipv4), bound,
                {reinterpret_cast<const std::uint8_t*>(garbage.data()), garbage.size()});
        std::this_thread::sleep_for(400ms);
        CHECK(listener.rejected_datagrams() >= 3);
        view = listener.snapshot();
        REQUIRE(view);
        CHECK_EQ(view->size(), 1U); // la session valide survit au bruit.
        sender.close();
    }

    // Saisie manuelle : secours sans découverte, sans identifiant de session.
    listener.inject_manual(IpAddress::loopback(IpFamily::Ipv4), Port{40001}, "session saisie à la main");
    view = listener.snapshot();
    REQUIRE(view);
    CHECK_EQ(view->size(), 2U);
    bool found_manual = false;
    for (const auto& entry : *view) {
        if (!entry.manual) continue;
        found_manual = true;
        CHECK_EQ(entry.session_id, 0U);
        CHECK_EQ(entry.session_name, std::string("session saisie à la main"));
        CHECK_EQ(entry.control_port, 40001);
        CHECK(!entry.expired(clock.now_monotonic_ms() + 60000)); // une saisie manuelle n'expire pas.
    }
    CHECK(found_manual);
    listener.stop();
    CHECK(!listener.running());
}
}

void lan_resume_secret_binds_identity() {
    SystemClock clock;
    auto code = generate_invite_code(); REQUIRE(code);
    HostServer host(clock);
    HostConfig config;
    config.session_name = "Reprise";
    config.host_name = "poste-hôte";
    config.bind_address = IpAddress::loopback(IpFamily::Ipv4);
    config.approval_required = false;   // admission directe : le test porte sur la reprise.
    config.max_participants = 4;
    config.reconnect_grace_ms = 60000;
    config.invite_code = code.value();
    auto port = host.start(config); REQUIRE(port);

    const auto base = [&] {
        GuestConfig guest;
        guest.address = IpAddress::loopback(IpFamily::Ipv4);
        guest.control_port = port.value();
        guest.invite_code = code.value();
        return guest;
    };
    const auto connected = [](const GuestSnapshot& snapshot) { return snapshot.phase == GuestPhase::Connected; };

    // 1. Première admission : un secret de reprise est délivré sur le canal authentifié.
    GuestConfig first = base(); first.guest_name = "Alice";
    GuestClient alice(clock);
    REQUIRE(alice.start(first));
    REQUIRE(alice.wait_for(connected, 10s));
    const auto alice_id = alice.snapshot()->self;
    CHECK(alice_id.value != 0);
    const auto secret = alice.resume_secret();
    CHECK_EQ(secret.size(), kResumeSecretChars);
    CHECK(alice.snapshot()->resume_available);
    CHECK_EQ(alice.snapshot()->resume_ttl_ms, config.reconnect_grace_ms);
    for (const auto& event : alice.snapshot()->events) CHECK(event.find(secret) == std::string::npos);
    for (const auto& event : host.snapshot()->events) CHECK(event.find(secret) == std::string::npos);

    // 2. Coupure brute (socket fermée sans Bye) : la place est réservée.
    alice.stop();
    REQUIRE(host.wait_for([&](const HostSnapshot& snapshot) {
        for (const auto& peer : snapshot.participants)
            if (peer.id == alice_id) return peer.state == plpt::model::ParticipantState::Disconnected;
        return false;
    }, 10s));

    // 3. Un homonyme sans secret n'hérite de rien : le nom n'identifie personne.
    GuestConfig twin = base(); twin.guest_name = "Alice";
    GuestClient homonym(clock);
    REQUIRE(homonym.start(twin));
    REQUIRE(homonym.wait_for(connected, 10s));
    CHECK(homonym.snapshot()->self != alice_id);
    CHECK_EQ(host.snapshot()->counters.resumed, 0U);
    for (const auto& event : host.snapshot()->events) CHECK(event.find("a repris sa place") == std::string::npos);

    // 4. Un secret inventé ne vaut rien non plus.
    GuestConfig forged = base(); forged.guest_name = "Alice"; forged.resume_secret = std::string(kResumeSecretChars, '0');
    GuestClient impostor(clock);
    REQUIRE(impostor.start(forged));
    REQUIRE(impostor.wait_for(connected, 10s));
    CHECK(impostor.snapshot()->self != alice_id);
    CHECK_EQ(host.snapshot()->counters.resumed, 0U);

    // 5. Le vrai secret rend SA place, puis il est consommé (rotation).
    GuestConfig back = base(); back.guest_name = "Alice"; back.resume_secret = secret;
    GuestClient resumed(clock);
    REQUIRE(resumed.start(back));
    REQUIRE(resumed.wait_for(connected, 10s));
    CHECK(resumed.snapshot()->self == alice_id);
    CHECK(host.snapshot()->counters.resumed >= 1);
    CHECK(!resumed.resume_secret().empty());
    CHECK(resumed.resume_secret() != secret);
    CHECK(host.wait_for([](const HostSnapshot& snapshot) {
        for (const auto& event : snapshot.events)
            if (event.find("a repris sa place") != std::string::npos) return true;
        return false;
    }, 10s));

    // 6. Départ volontaire : la place est libérée, aucune reprise n'est réservée.
    CHECK(resumed.leave());
    CHECK(host.wait_for([&](const HostSnapshot& snapshot) {
        for (const auto& peer : snapshot.participants)
            if (peer.id == alice_id) return peer.state == plpt::model::ParticipantState::Revoked;
        return false;
    }, 10s));
    const auto rotated = resumed.resume_secret();
    resumed.stop();

    // 7. Après libération, le secret précédent ne rend plus rien.
    GuestConfig after = base(); after.guest_name = "Alice"; after.resume_secret = rotated;
    GuestClient late(clock);
    REQUIRE(late.start(after));
    CHECK(late.wait_for([](const GuestSnapshot& snapshot) {
        return snapshot.phase == GuestPhase::Connected || snapshot.phase == GuestPhase::Refused ||
            snapshot.phase == GuestPhase::Disconnected;
    }, 10s));
    CHECK(late.snapshot()->self != alice_id);
    for (const auto& event : host.snapshot()->events) CHECK(event.find(rotated) == std::string::npos);

    late.stop(); impostor.stop(); homonym.stop();
    host.stop();
    CHECK(!host.running());
}


void lan_proxy_informational_and_upgrade_responses() {
    SystemClock clock;
    auto token = generate_service_token(); REQUIRE(token);

    {   // 100 Continue puis 103 Early Hints puis réponse finale : le cookie n'est
        // déposé que dans la réponse pertinente, les 1xx passent telles quelles.
        TinyUpstream upstream;
        const std::string prelude = "HTTP/1.1 100 Continue\r\n\r\n"
            "HTTP/1.1 103 Early Hints\r\nLink: </app.css>; rel=preload; as=style\r\n\r\n";
        REQUIRE(upstream.start("page réelle", prelude, std::string()));
        ServiceProxy proxy(clock);
        auto started = proxy.start(loopback_proxy_for(upstream, token.value())); REQUIRE(started);
        const auto response = http_exchange(started.value(), "/?plpt=" + token.value(), "");
        CHECK(response.find("HTTP/1.1 100 Continue\r\n\r\n") == 0);
        const auto hints = response.find("HTTP/1.1 103 Early Hints");
        const auto final_head = response.find("HTTP/1.1 200 OK");
        CHECK(hints != std::string::npos);
        CHECK(final_head != std::string::npos);
        CHECK(hints < final_head);
        CHECK(response.find("Link: </app.css>; rel=preload; as=style") != std::string::npos);
        // Aucun cookie dans la partie informationnelle.
        CHECK(response.substr(0, final_head).find("Set-Cookie:") == std::string::npos);
        const auto cookie = response.find("Set-Cookie: plpt=" + token.value());
        CHECK(cookie != std::string::npos);
        CHECK(cookie > final_head);
        CHECK(response.find("page réelle") != std::string::npos);
        // Le service local n'a jamais vu le jeton.
        CHECK(upstream.last_request().find(token.value()) == std::string::npos);
        CHECK(upstream.last_request().find("GET / HTTP/1.1") != std::string::npos);
        const auto view = proxy.snapshot();
        CHECK_EQ(view->counters.informational_responses, 2U);
        CHECK_EQ(view->counters.grants_issued, 1U);   // une seule réponse pertinente
        CHECK_EQ(view->counters.grants_skipped, 0U);
        CHECK_EQ(view->counters.grants_failed, 0U);
        CHECK_EQ(view->counters.tokens_redacted, 1U);
        proxy.stop();
    }
    {   // Upgrade WebSocket : la réponse 101 n'est jamais modifiée.
        TinyUpstream upstream;
        const std::string raw = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        REQUIRE(upstream.start(std::string(), std::string(), raw));
        ServiceProxy proxy(clock);
        auto started = proxy.start(loopback_proxy_for(upstream, token.value())); REQUIRE(started);
        const auto response = http_exchange(started.value(), "/ws?plpt=" + token.value(),
            "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n");
        CHECK(response.find("HTTP/1.1 101 Switching Protocols\r\n") == 0);
        CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        CHECK(response.find("Set-Cookie:") == std::string::npos);
        const auto view = proxy.snapshot();
        CHECK(view->counters.websocket_upgrades >= 1);
        CHECK_EQ(view->counters.grants_issued, 0U);
        CHECK_EQ(view->counters.grants_skipped, 0U); // interception jamais engagée
        CHECK_EQ(view->counters.tokens_redacted, 1U);
        CHECK(upstream.last_request().find(token.value()) == std::string::npos);
        CHECK(upstream.last_request().find("GET /ws HTTP/1.1") != std::string::npos);
        CHECK(upstream.last_request().find("Sec-WebSocket-Key") != std::string::npos);
        proxy.stop();
    }
    {   // 101 sur une requête SANS upgrade : renoncement explicite, cookie non délivré.
        TinyUpstream upstream;
        const std::string raw = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        REQUIRE(upstream.start(std::string(), std::string(), raw));
        ServiceProxy proxy(clock);
        auto started = proxy.start(loopback_proxy_for(upstream, token.value())); REQUIRE(started);
        const auto response = http_exchange(started.value(), "/?plpt=" + token.value(), "");
        CHECK(response.find("HTTP/1.1 101 Switching Protocols\r\n") == 0);
        CHECK(response.find("Set-Cookie:") == std::string::npos);
        const auto view = proxy.snapshot();
        CHECK_EQ(view->counters.grants_skipped, 1U);
        CHECK_EQ(view->counters.grants_issued, 0U);
        CHECK(!view->warnings.empty());
        proxy.stop();
    }
    {   // Réponse amont illisible : transmise telle quelle, aucun cookie, avertissement.
        TinyUpstream upstream;
        REQUIRE(upstream.start(std::string(), std::string(), "PAS-UNE-REPONSE\r\n\r\n"));
        ServiceProxy proxy(clock);
        auto started = proxy.start(loopback_proxy_for(upstream, token.value())); REQUIRE(started);
        const auto response = http_exchange(started.value(), "/?plpt=" + token.value(), "");
        CHECK(response.find("PAS-UNE-REPONSE") == 0);
        CHECK(response.find("Set-Cookie:") == std::string::npos);
        const auto view = proxy.snapshot();
        CHECK_EQ(view->counters.grants_skipped, 1U);
        CHECK_EQ(view->counters.grants_issued, 0U);
        CHECK(!view->warnings.empty());
        proxy.stop();
    }
}

void pool_saturation_with_held_open_connections() {
    // Capacité explicite : 2 connexions ACTIVES et 2 tâches EN ATTENTE, soit 4 au
    // total. Les connexions sont réelles et MAINTENUES OUVERTES : le traitement
    // bloque tant que le client n'écrit pas, donc la saturation est observée, pas
    // supposée.
    auto created_listener = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
    REQUIRE(created_listener);
    auto listener = std::move(created_listener).value();
    REQUIRE(listener.bind(IpAddress::loopback(IpFamily::Ipv4), Port{0}));
    auto bound = listener.bound_port(); REQUIRE(bound);
    REQUIRE(listener.listen(16));

    ConnectionPool pool;
    REQUIRE(pool.start(2, 2, [](Socket& socket, const IpAddress&) {
        std::array<std::uint8_t, 1> byte{};
        (void)read_some(socket, byte, std::chrono::milliseconds(10000)); // connexion tenue ouverte
        const std::string done = "OK\n";
        (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(done.data()), done.size()},
            std::chrono::milliseconds(2000));
    }));
    CHECK(pool.running());
    CHECK_EQ(pool.capacity(), 4U);

    std::vector<Socket> clients;
    std::vector<Socket> refused_sockets;
    unsigned active_submitted = 0, queued_submitted = 0, refused_submitted = 0;
    for (int index = 0; index < 6; ++index) {
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(created);
        auto client = std::move(created).value();
        REQUIRE(connect_with_timeout(client, IpAddress::loopback(IpFamily::Ipv4), bound.value(), std::chrono::milliseconds(3000)));
        IpAddress peer;
        Port peer_port{};
        auto accepted = listener.accept(&peer, &peer_port); REQUIRE(accepted);
        // En cas de refus la socket n'est pas consommée : elle reste à l'appelant.
        switch (pool.submit(std::move(accepted).value(), peer)) {
        case PoolSubmission::Active: ++active_submitted; break;
        case PoolSubmission::Queued: ++queued_submitted; break;
        case PoolSubmission::Refused:
            ++refused_submitted;
            refused_sockets.push_back(std::move(accepted).value());
            break;
        }
        clients.push_back(std::move(client));
    }
    CHECK_EQ(active_submitted, 2U);
    CHECK_EQ(queued_submitted, 2U);
    CHECK_EQ(refused_submitted, 2U);

    // Les deux notions sont observées séparément, sur état réel et dans un délai borné.
    REQUIRE(pool.wait_for([](std::size_t active, std::size_t queued, const PoolCounters&) {
        return active == 2 && queued == 2;
    }, std::chrono::milliseconds(5000)));
    CHECK_EQ(pool.active(), 2U);
    CHECK_EQ(pool.queued(), 2U);
    {
        const auto counters = pool.counters();
        CHECK_EQ(counters.submitted, 4U);
        CHECK_EQ(counters.refused, 2U);
        CHECK_EQ(counters.completed, 0U);   // rien n'est terminé : tout est tenu ouvert
        CHECK_EQ(counters.max_queued, 2U);
    }
    // Tant que rien ne se libère, toute soumission supplémentaire est refusée.
    {
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(created);
        auto extra = std::move(created).value();
        REQUIRE(connect_with_timeout(extra, IpAddress::loopback(IpFamily::Ipv4), bound.value(), std::chrono::milliseconds(3000)));
        IpAddress peer;
        Port peer_port{};
        auto accepted = listener.accept(&peer, &peer_port); REQUIRE(accepted);
        CHECK(pool.submit(std::move(accepted).value(), peer) == PoolSubmission::Refused);
        accepted.value().close();
        extra.close();
    }
    // Libération des deux connexions actives : les tâches en attente prennent la suite.
    const std::string go = "x";
    for (std::size_t index = 0; index < 2; ++index)
        CHECK(write_all(clients[index], {reinterpret_cast<const std::uint8_t*>(go.data()), go.size()},
            std::chrono::milliseconds(2000)));
    REQUIRE(pool.wait_for([](std::size_t active, std::size_t queued, const PoolCounters& counters) {
        return counters.completed == 2 && active == 2 && queued == 0;
    }, std::chrono::milliseconds(5000)));
    for (std::size_t index = 2; index < 4; ++index)
        CHECK(write_all(clients[index], {reinterpret_cast<const std::uint8_t*>(go.data()), go.size()},
            std::chrono::milliseconds(2000)));
    REQUIRE(pool.wait_for([](std::size_t active, std::size_t queued, const PoolCounters& counters) {
        return counters.completed == 4 && active == 0 && queued == 0;
    }, std::chrono::milliseconds(5000)));
    for (std::size_t index = 0; index < 4; ++index) {
        std::array<char, 8> reply{};
        const auto got = read_some(clients[index], {reinterpret_cast<std::uint8_t*>(reply.data()), reply.size()},
            std::chrono::milliseconds(2000));
        REQUIRE(got);
        CHECK(std::string(reply.data(), got.value()).find("OK") != std::string::npos);
    }
    {
        const auto counters = pool.counters();
        CHECK_EQ(counters.submitted, 4U);
        CHECK_EQ(counters.completed, 4U);
        CHECK_EQ(counters.refused, 3U);
        CHECK_EQ(counters.max_active, 2U);   // jamais plus d'ouvriers que configuré
        CHECK_EQ(counters.max_queued, 2U);
    }
    // Arrêt : tous les ouvriers sont joints, plus aucune soumission n'est acceptée.
    for (auto& client : clients) client.close();
    for (auto& refused_socket : refused_sockets) refused_socket.close();
    listener.close();
    pool.stop();
    CHECK(!pool.running());
    CHECK_EQ(pool.active(), 0U);
    CHECK_EQ(pool.queued(), 0U);
    {   // Un pool arrêté refuse, il ne silently ignore pas.
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4); REQUIRE(created);
        auto socket = std::move(created).value();
        CHECK(pool.submit(std::move(socket), IpAddress::loopback(IpFamily::Ipv4)) == PoolSubmission::Refused);
        CHECK_EQ(pool.counters().refused, 4U);
    }
}

// ---------------------------------------------------------------------------
// Transport versionné (mandat §24) : TCP clair déclaré, TLS 1.3 PSK réel.
// ---------------------------------------------------------------------------

#include "engine/net/transport.hpp"

using plpt::net::ITransportChannel;
using plpt::net::ITransportEndpoint;
using plpt::net::ITransportFactory;
using plpt::net::TransportCredentials;
using plpt::net::TransportInfo;
using plpt::net::TransportKind;

namespace {
const TransportCredentials kTlsCredentials{"devlink-session-test", "secret-hors-bande-0123456789"};

std::string exchange_round_trip(ITransportChannel& channel, std::string_view payload) {
    const auto sent = channel.send(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    if (!sent) return std::string("envoi en echec : ") + sent.error().message;
    std::uint8_t buffer[256] = {};
    const auto got = channel.receive(buffer);
    if (!got) return std::string("reception en echec : ") + got.error().message;
    return std::string(reinterpret_cast<char*>(buffer), got.value());
}
} // namespace

void transport_clear_declares_itself() {
    auto factory = plpt::net::make_clear_tcp_factory();
    CHECK(factory.has_value());
    if (!factory.has_value()) return;
    CHECK_EQ(factory.value()->interface_version(), plpt::net::kTransportInterfaceVersion);
    CHECK(factory.value()->kind() == TransportKind::ClearTcp);
    CHECK(factory.value()->available());
    auto endpoint = factory.value()->listen("127.0.0.1", kTlsCredentials);
    CHECK(endpoint.has_value());
    if (!endpoint.has_value()) return;
    std::string server_report;
    std::thread server([&] {
        auto channel = endpoint.value()->accept();
        if (!channel) { server_report = "accept clair en echec : " + channel.error().message; return; }
        const TransportInfo info = channel.value()->info();
        server_report = std::string(info.protocol) + "|" + (info.peer_authenticated ? "auth" : "non-auth") +
            "|" + info.cipher + "|" + exchange_round_trip(*channel.value(), "ping-clair");
    });
    auto client = factory.value()->connect("127.0.0.1", endpoint.value()->port(), kTlsCredentials);
    CHECK(client.has_value());
    if (client.has_value()) {
        const TransportInfo info = client.value()->info();
        CHECK(info.protocol == "tcp clair");
        CHECK(!info.peer_authenticated); // le clair ne pretend JAMAIS authentifier
        CHECK(info.cipher.empty());
        CHECK_EQ(exchange_round_trip(*client.value(), "ping-clair"), std::string("ping-clair"));
    }
    server.join();
    CHECK_EQ(server_report, std::string("tcp clair|non-auth||ping-clair"));
    endpoint.value()->close();
}

void tls13_psk_loopback_authenticated() {
    auto factory = plpt::net::make_tls13_factory();
    CHECK(factory.has_value());
    if (!factory.has_value()) return;
    CHECK(factory.value()->available());
    CHECK_EQ(factory.value()->kind(), TransportKind::Tls13Psk);
    auto endpoint = factory.value()->listen("127.0.0.1", kTlsCredentials);
    CHECK(endpoint.has_value());
    if (!endpoint.has_value()) return;
    std::string server_report;
    bool server_authenticated = false;
    std::thread server([&] {
        auto channel = endpoint.value()->accept();
        if (!channel) { server_report = "accept tls en echec : " + channel.error().message; return; }
        const TransportInfo info = channel.value()->info();
        server_authenticated = info.peer_authenticated;
        server_report = info.protocol + "|" + info.cipher + "|" +
            exchange_round_trip(*channel.value(), "marqueur-tls-serveur");
    });
    auto client = factory.value()->connect("127.0.0.1", endpoint.value()->port(), kTlsCredentials);
    CHECK(client.has_value());
    if (client.has_value()) {
        const TransportInfo info = client.value()->info();
        CHECK(info.protocol == "TLSv1.3");
        CHECK(info.peer_authenticated);
        CHECK(info.cipher.find("TLS_AES") == 0);
        CHECK_EQ(exchange_round_trip(*client.value(), "marqueur-tls-client"), std::string("marqueur-tls-serveur"));
    }
    server.join();
    CHECK(server_authenticated);
    CHECK(server_report.rfind("TLSv1.3|", 0) == 0);
    CHECK(server_report.find("marqueur-tls-client") != std::string::npos);
    endpoint.value()->close();
}

void tls13_psk_wrong_secret_refused() {
    auto factory = plpt::net::make_tls13_factory();
    CHECK(factory.has_value());
    if (!factory.has_value()) return;
    auto endpoint = factory.value()->listen("127.0.0.1", kTlsCredentials);
    CHECK(endpoint.has_value());
    if (!endpoint.has_value()) return;
    bool server_refused = false;
    std::thread server([&] {
        auto channel = endpoint.value()->accept();
        server_refused = !channel.has_value();
        if (channel.has_value()) channel.value()->close(); // ne doit JAMAIS arriver
    });
    const TransportCredentials impostor{"devlink-session-test", "mauvais-secret-0123456789!!"};
    auto client = factory.value()->connect("127.0.0.1", endpoint.value()->port(), impostor);
    CHECK(!client.has_value());
    if (!client.has_value()) {
        CHECK(client.error().code == plpt::ErrorCode::Unauthorized ||
              client.error().code == plpt::ErrorCode::Disconnected);
    }
    server.join();
    CHECK(server_refused); // poignée de main refusée des deux côtés, aucun canal rendu
    endpoint.value()->close();
}

void tls13_downgrade_refused() {
    // Politique incohérente (min > max) : refusée AVANT toute écoute, sans thread.
    plpt::net::TlsPolicy inverted;
    inverted.min_version = plpt::net::kTls13Version;
    inverted.max_version = plpt::net::kTls12Version;
    auto rejected = plpt::net::make_tls13_factory(inverted);
    CHECK(!rejected.has_value()); // jamais de fabrique silencieusement « corrigée »

    // Client qui n'offre que TLS 1.2 face au serveur TLS 1.3 strict : refus des
    // deux côtés, aucun canal, aucun repli.
    auto server_factory = plpt::net::make_tls13_factory();
    CHECK(server_factory.has_value());
    if (!server_factory.has_value()) return;
    auto endpoint = server_factory.value()->listen("127.0.0.1", kTlsCredentials);
    CHECK(endpoint.has_value());
    if (!endpoint.has_value()) return;
    bool server_refused = false;
    std::thread server([&] {
        auto channel = endpoint.value()->accept();
        server_refused = !channel.has_value();
        if (channel.has_value()) channel.value()->close(); // ne doit JAMAIS arriver
    });
    plpt::net::TlsPolicy legacy;
    legacy.min_version = plpt::net::kTls12Version;
    legacy.max_version = plpt::net::kTls12Version;
    auto client_factory = plpt::net::make_tls13_factory(legacy);
    CHECK(client_factory.has_value());
    if (client_factory.has_value()) {
        auto client = client_factory.value()->connect("127.0.0.1", endpoint.value()->port(), kTlsCredentials);
        CHECK(!client.has_value()); // le serveur TLS 1.3 strict refuse la négociation 1.2
    }
    server.join();
    CHECK(server_refused);
    endpoint.value()->close();
}

void tls13_availability_is_honest() {
    std::string reason;
    const bool available = plpt::net::tls13_runtime_available(&reason);
    const char* forced = std::getenv("DEVLINK_TLS_LIBRARY_PATH");
    auto factory = plpt::net::make_tls13_factory();
    CHECK(factory.has_value());
    if (!factory.has_value()) return;
    if (forced != nullptr) {
        // Variable de test : bibliothèque volontairement introuvable. La fabrique
        // doit le dire, et refuser listen/connect sans jamais replier sur du clair.
        CHECK(!available);
        CHECK(!reason.empty());
        CHECK(!factory.value()->available());
        CHECK(factory.value()->unavailability_reason() == reason);
        auto listen = factory.value()->listen("127.0.0.1", kTlsCredentials);
        CHECK(!listen.has_value());
        if (!listen.has_value()) CHECK_EQ(listen.error().code, plpt::ErrorCode::Unsupported);
        std::printf("tls-unavailable-proof: reason=%s\n", reason.c_str());
    } else {
        CHECK_EQ(factory.value()->available(), available);
        if (!available) CHECK(!reason.empty()); // indisponible doit savoir pourquoi
        std::printf("tls13 runtime: available=%d reason=%s\n", available ? 1 : 0, reason.c_str());
    }
}

#ifndef _WIN32
#include <poll.h>
// Oracle d'écoute : un relais octet-par-octet enregistre tout ce qui traverse.
// Le marqueur clair doit être ABSENT de l'enregistrement : la charge utile est
// réellement chiffrée de bout en bout, pas simplement déclarée telle.
void tls13_wire_oracle_no_plaintext() {
    auto factory = plpt::net::make_tls13_factory();
    CHECK(factory.has_value());
    if (!factory.has_value()) return;
    auto endpoint = factory.value()->listen("127.0.0.1", kTlsCredentials);
    CHECK(endpoint.has_value());
    if (!endpoint.has_value()) return;

    const int relay_listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(relay_listener >= 0);
    sockaddr_in relay_address{};
    relay_address.sin_family = AF_INET;
    relay_address.sin_port = 0;
    CHECK_EQ(::inet_pton(AF_INET, "127.0.0.1", &relay_address.sin_addr), 1);
    CHECK_EQ(::bind(relay_listener, reinterpret_cast<sockaddr*>(&relay_address), sizeof(relay_address)), 0);
    CHECK_EQ(::listen(relay_listener, 1), 0);
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    CHECK_EQ(::getsockname(relay_listener, reinterpret_cast<sockaddr*>(&bound), &bound_len), 0);
    const std::uint16_t relay_port = static_cast<std::uint16_t>(ntohs(bound.sin_port));

    std::mutex wire_mutex;
    std::string wire;
    std::atomic<bool> relay_done{false};
    const std::uint16_t tls_port = endpoint.value()->port();
    std::thread relay([&] {
        const int client_fd = ::accept(relay_listener, nullptr, nullptr);
        if (client_fd < 0) { relay_done.store(true); return; }
        sockaddr_in upstream{};
        upstream.sin_family = AF_INET;
        upstream.sin_port = htons(tls_port);
        ::inet_pton(AF_INET, "127.0.0.1", &upstream.sin_addr);
        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (::connect(server_fd, reinterpret_cast<sockaddr*>(&upstream), sizeof(upstream)) != 0) {
            ::close(client_fd); relay_done.store(true); return;
        }
        pollfd watches[2] = {{client_fd, POLLIN, 0}, {server_fd, POLLIN, 0}};
        char buffer[2048];
        for (;;) {
            const int ready = ::poll(watches, 2, 2000);
            if (ready <= 0) break;
            bool closed = false;
            for (int side = 0; side < 2 && !closed; ++side) {
                if (watches[side].revents & POLLIN) {
                    const int got = ::recv(watches[side].fd, buffer, sizeof(buffer), 0);
                    if (got <= 0) { closed = true; break; }
                    {
                        std::lock_guard<std::mutex> lock(wire_mutex);
                        wire.append(buffer, static_cast<std::size_t>(got));
                    }
                    const int peer = side == 0 ? server_fd : client_fd;
                    int offset = 0;
                    while (offset < got) {
                        const int sent = ::send(peer, buffer + offset, got - offset, 0);
                        if (sent <= 0) { closed = true; break; }
                        offset += sent;
                    }
                }
            }
            if (closed) break;
        }
        ::close(client_fd);
        ::close(server_fd);
        relay_done.store(true);
    });

    std::thread server([&] {
        auto channel = endpoint.value()->accept();
        if (!channel) return;
        std::uint8_t buffer[256] = {};
        const auto got = channel.value()->receive(buffer);
        if (got) {
            const auto echo = channel.value()->send(std::span<const std::uint8_t>(buffer, got.value()));
            if (!echo) std::printf("oracle: echo tls en echec : %s\n", echo.error().message.c_str());
        }
    });

    auto client = factory.value()->connect("127.0.0.1", relay_port, kTlsCredentials);
    CHECK(client.has_value());
    if (client.has_value()) {
        CHECK_EQ(exchange_round_trip(*client.value(), "MARQUEUR-CLAIR-DEVLINK-4242"),
            std::string("MARQUEUR-CLAIR-DEVLINK-4242"));
        client.value()->close();
    }
    server.join();
    for (int wait = 0; wait < 100 && !relay_done.load(); ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(relay_listener);
    relay.join();
    std::string captured;
    {
        std::lock_guard<std::mutex> lock(wire_mutex);
        captured = wire;
    }
    CHECK(captured.size() > 200); // du trafic a réellement traversé l'oracle
    CHECK(captured.find("MARQUEUR-CLAIR-DEVLINK-4242") == std::string::npos); // jamais en clair
    CHECK(captured.find("DevLink TLS13 PSK v1") == std::string::npos); // ni le libellé de dérivation
    endpoint.value()->close();
}
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--ipv6") {
        DynamicPortAllocator allocator;
        const auto probe = allocator.acquire(TransportProtocol::Udp, IpFamily::Ipv6);
        if (!probe) { std::printf("SKIP IPv6 unavailable, native error=%lld\n", static_cast<long long>(probe.error().native_code)); return 77; }
        CASE(ipv6_allocation); CASE(ipv6_native_tables_and_process);
        return plpt_test::finish("ipv6");
    }
    CASE(entropy_os); CASE(ipv4_allocation); CASE(ipv4_native_tables_and_process); CASE(interface_addresses);
#ifndef _WIN32
    CASE(proc_parsers_and_fixtures); CASE(repeated_allocation_releases_fds);
#endif
    CASE(lan_control_plane_host_and_guest); CASE(lan_data_plane_proxy_token_gate);
    CASE(lan_resume_secret_binds_identity);
    CASE(lan_discovery_announce_and_listen);
    CASE(lan_proxy_informational_and_upgrade_responses); CASE(pool_saturation_with_held_open_connections);
    CASE(transport_clear_declares_itself);
    CASE(tls13_availability_is_honest);
    {
        std::string tls_reason;
        const bool tls_available = plpt::net::tls13_runtime_available(&tls_reason);
        if (!tls_available) {
            // SKIP n'est pas un PASS : les cas TLS ne sont pas comptes ici.
            std::printf("SKIP tls13: runtime indisponible (%s)\n", tls_reason.c_str());
        } else {
            CASE(tls13_psk_loopback_authenticated);
            CASE(tls13_psk_wrong_secret_refused);
            CASE(tls13_downgrade_refused);
#ifndef _WIN32
            CASE(tls13_wire_oracle_no_plaintext);
#endif
        }
    }
    return plpt_test::finish("platform");
}
