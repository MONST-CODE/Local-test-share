#include <array>
#include <fstream>
#include <filesystem>
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
    bool start(std::string body) {
        body_ = std::move(body);
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
            // La ligne de requête est renvoyée dans le corps : cela prouve que le
            // proxy transmet la requête originale, jeton compris, sans la réécrire.
            const auto request_line = request.substr(0, request.find("\r\n"));
            const std::string payload = body_ + "\n" + request_line;
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
        CHECK_EQ(view->pending[0].client, std::string("PLPT 1.1"));
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

    const auto request = [&](const std::string& target, const std::string& headers) {
        auto created = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
        if (!created) return std::string();
        auto socket = std::move(created).value();
        if (!connect_with_timeout(socket, IpAddress::loopback(IpFamily::Ipv4), Port{started.value()}, 3s)) return std::string();
        const std::string text = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n" + headers + "\r\n";
        if (!write_all(socket, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, 3s)) return std::string();
        std::string raw; std::vector<std::uint8_t> buffer(8192);
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto received = socket.receive(buffer);
            if (!received || received.value() == 0) break;
            raw.append(reinterpret_cast<const char*>(buffer.data()), received.value());
        }
        socket.close();
        return raw;
    };
    {
        const auto response = request("/?plpt=" + token.value(), "");
        CHECK(response.find("HTTP/1.1 200") == 0);
        CHECK(response.find("contenu réel du service local") != std::string::npos);
        CHECK(response.find("/?plpt=") != std::string::npos); // requête transmise telle quelle.
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
    CHECK(request("/", "X-PLPT-Token: " + token.value() + "\r\n").find("HTTP/1.1 200") == 0);
    CHECK(request("/", "Cookie: plpt=" + token.value() + "\r\n").find("HTTP/1.1 200") == 0);
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
        CHECK(view->counters.authorized >= 3);
        CHECK(view->counters.refused >= 2);
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
    CASE(lan_discovery_announce_and_listen);
    return plpt_test::finish("platform");
}
