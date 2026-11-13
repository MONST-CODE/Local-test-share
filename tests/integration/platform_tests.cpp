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
    return plpt_test::finish("platform");
}
