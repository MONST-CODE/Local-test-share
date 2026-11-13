#include "linux_platform.hpp"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace plpt {
namespace {
template <typename T> bool number(std::string_view text, T& value, int base = 10) {
    if (text.empty()) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return ec == std::errc{} && end == text.data() + text.size();
}
std::vector<std::string> fields(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::vector<std::string> out;
    std::string field;
    while (out.size() < 64 && input >> field) out.push_back(std::move(field));
    return out;
}
Error malformed(const char* operation) { return {ErrorCode::ProtocolViolation, "ligne proc invalide", operation}; }
Result<Endpoint> endpoint(std::string_view text, IpFamily family) {
    const std::size_t chars = family == IpFamily::Ipv4 ? 8 : 32;
    if (text.size() != chars + 5 || text[chars] != ':') return Result<Endpoint>::failure(malformed("endpoint"));
    std::uint16_t port_value = 0;
    if (!number(text.substr(chars + 1), port_value, 16)) return Result<Endpoint>::failure(malformed("port"));
    std::array<std::uint8_t, 16> bytes{};
    // /proc expose des mots de 32 bits dans l'ordre natif de l'hôte. memcpy
    // reconstitue les octets réseau sans présumer little-endian.
    for (std::size_t word = 0; word < chars / 8; ++word) {
        std::uint32_t value = 0;
        if (!number(text.substr(word * 8, 8), value, 16)) return Result<Endpoint>::failure(malformed("address"));
        std::memcpy(bytes.data() + word * 4, &value, sizeof(value));
    }
    char buffer[INET6_ADDRSTRLEN]{};
    if (!inet_ntop(family == IpFamily::Ipv4 ? AF_INET : AF_INET6, bytes.data(), buffer, sizeof(buffer)))
        return Result<Endpoint>::failure({ErrorCode::PlatformError, "inet_ntop", "endpoint", errno});
    return Result<Endpoint>::success({family, buffer, Port{port_value}});
}
void incomplete(Observation<SocketRecord>& out, const char* operation, std::int64_t code = 0) {
    out.complete = false;
    if (out.warnings.size() < 8) out.warnings.push_back({ErrorCode::IoError, "table inaccessible ou partielle", operation, code});
}
} // namespace

Result<ProcSocketRow> parse_proc_socket(std::string_view line, TransportProtocol proto, IpFamily family) {
    if ((proto != TransportProtocol::Tcp && proto != TransportProtocol::Udp) ||
        (family != IpFamily::Ipv4 && family != IpFamily::Ipv6) || line.size() > 8192)
        return Result<ProcSocketRow>::failure(malformed("socket"));
    const auto f = fields(line);
    // Colonnes tokenisées : sl local remote st queues timer retrans uid timeout inode.
    if (f.size() < 10) return Result<ProcSocketRow>::failure(malformed("socket"));
    auto local = endpoint(f[1], family);
    auto remote = endpoint(f[2], family);
    std::uint32_t state = 0;
    std::uint64_t inode = 0;
    if (!local || !remote || !number(f[3], state, 16) || !number(f[9], inode))
        return Result<ProcSocketRow>::failure(malformed("socket"));
    SocketState normalized = SocketState::Connected;
    if (proto == TransportProtocol::Udp) {
        normalized = remote.value().port.value == 0 ? SocketState::Bound : SocketState::Connected;
    } else {
        switch (state) {
        case 0x0A: normalized = SocketState::Listening; break;
        case 0x01: normalized = SocketState::Established; break;
        case 0x06: normalized = SocketState::TimeWait; break;
        case 0x07: normalized = SocketState::Closed; break;
        default: break;
        }
    }
    return Result<ProcSocketRow>::success({{proto, std::move(local).value(), std::move(remote).value(), normalized, std::nullopt}, inode});
}

Result<ProcessRecord> parse_proc_stat(std::string_view line) {
    if (line.size() > 8192) return Result<ProcessRecord>::failure(malformed("stat"));
    const auto open = line.find('('), close = line.rfind(')');
    if (open < 2 || close == std::string_view::npos || close <= open || open == std::string_view::npos)
        return Result<ProcessRecord>::failure(malformed("stat"));
    ProcessRecord record;
    if (!number(line.substr(0, open - 1), record.pid.value) || record.pid.value == 0)
        return Result<ProcessRecord>::failure(malformed("stat.pid"));
    record.image_name = std::string(line.substr(open + 1, close - open - 1));
    const auto f = fields(line.substr(close + 1)); // f[0] = champ 3 ; f[19] = champ 22 (starttime).
    if (f.size() < 20 || !number(f[1], record.ppid.value) || !number(f[19], record.start_time_ticks))
        return Result<ProcessRecord>::failure(malformed("stat.fields"));
    return Result<ProcessRecord>::success(std::move(record));
}

Result<Observation<SocketRecord>> LinuxSocketTableProvider::snapshot() {
    Observation<SocketRecord> out;
    std::vector<ProcSocketRow> rows;
    for (const auto family : {IpFamily::Ipv4, IpFamily::Ipv6}) {
        for (const auto proto : {TransportProtocol::Tcp, TransportProtocol::Udp}) {
            std::string name = proto == TransportProtocol::Tcp ? "tcp" : "udp";
            if (family == IpFamily::Ipv6) name += '6';
            std::ifstream stream(fs::path(root_) / "net" / name);
            if (!stream) { incomplete(out, "proc.net.open", errno); continue; }
            std::string line;
            if (!std::getline(stream, line)) { incomplete(out, "proc.net.header"); continue; }
            bool invalid = false;
            while (std::getline(stream, line)) {
                auto row = parse_proc_socket(line, proto, family);
                if (!row) { invalid = true; continue; }
                if (rows.size() >= 262144) { invalid = true; break; }
                rows.push_back(std::move(row).value());
            }
            if (invalid || stream.bad()) incomplete(out, "proc.net.parse");
        }
    }
    // L'inode reste associé à SA ligne du même snapshot : aucune deuxième lecture
    // positionnelle des tables (elles peuvent changer à chaque lecture).
    std::map<std::uint64_t, std::set<ProcessId>> owners;
    std::error_code ec;
    fs::directory_iterator proc(root_, ec), end;
    if (ec) return Result<Observation<SocketRecord>>::failure({ErrorCode::IoError, "proc inaccessible", "sockets", ec.value()});
    for (; proc != end; proc.increment(ec)) {
        if (ec) break;
        ProcessId pid;
        if (!number(proc->path().filename().string(), pid.value) || pid.value == 0) continue;
        std::error_code fd_error;
        fs::directory_iterator fd(proc->path() / "fd", fd_error);
        if (fd_error) continue; // accès refusé/processus terminé : propriétaire inconnu.
        for (; fd != end; fd.increment(fd_error)) {
            if (fd_error) break;
            std::error_code link_error;
            const auto target = fs::read_symlink(fd->path(), link_error).string();
            if (link_error || !target.starts_with("socket:[") || !target.ends_with(']')) continue;
            std::uint64_t inode = 0;
            if (number(std::string_view(target).substr(8, target.size() - 9), inode) && inode != 0)
                owners[inode].insert(pid);
        }
    }
    // /proc/<pid>/fd n'est pas toujours lisible ; cela n'invalide pas la table réseau.
    for (auto& row : rows) {
        const auto it = owners.find(row.inode);
        if (it != owners.end() && it->second.size() == 1) row.socket.owner_pid = *it->second.begin();
        out.records.push_back(std::move(row.socket));
    }
    return Result<Observation<SocketRecord>>::success(std::move(out));
}

Result<Observation<ProcessRecord>> LinuxProcessEnumerator::enumerate() {
    Observation<ProcessRecord> out;
    std::error_code ec;
    fs::directory_iterator proc(root_, ec), end;
    if (ec) return Result<Observation<ProcessRecord>>::failure({ErrorCode::IoError, "proc inaccessible", "processes", ec.value()});
    for (; proc != end; proc.increment(ec)) {
        if (ec) break;
        ProcessId pid;
        if (!number(proc->path().filename().string(), pid.value) || pid.value == 0) continue;
        std::ifstream stream(proc->path() / "stat");
        if (!stream) continue;
        std::string line((std::istreambuf_iterator<char>(stream)), {});
        auto parsed = parse_proc_stat(line);
        if (!parsed || parsed.value().pid != pid) {
            out.complete = false;
            if (out.warnings.size() < 8) out.warnings.push_back(malformed("processes.stat"));
            continue;
        }
        auto record = std::move(parsed).value();
        std::error_code link_error;
        const auto path = fs::read_symlink(proc->path() / "exe", link_error);
        if (!link_error) record.executable_path = path.string();
        out.records.push_back(std::move(record));
    }
    if (ec) {
        out.complete = false;
        out.warnings.push_back({ErrorCode::IoError, "énumération interrompue", "processes", ec.value()});
    }
    return Result<Observation<ProcessRecord>>::success(std::move(out));
}

Result<std::vector<NetworkInterfaceInfo>> LinuxNetworkInterfaceProvider::enumerate() {
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw) != 0) return Result<std::vector<NetworkInterfaceInfo>>::failure(
        {ErrorCode::PlatformError, "getifaddrs", "interfaces", errno});
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> list(raw, &freeifaddrs);
    std::vector<NetworkInterfaceInfo> out;
    for (auto* p = raw; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        const auto fam = p->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        NetworkInterfaceInfo info;
        info.name = p->ifa_name;
        info.family = fam == AF_INET ? IpFamily::Ipv4 : IpFamily::Ipv6;
        info.is_up = (p->ifa_flags & IFF_UP) != 0;
        info.is_loopback = (p->ifa_flags & IFF_LOOPBACK) != 0;
        const void* address_ptr = nullptr;
        if (fam == AF_INET) address_ptr = &reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr;
        else {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(p->ifa_addr);
            address_ptr = &a->sin6_addr;
            info.scope_id = a->sin6_scope_id;
        }
        char buffer[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(fam, address_ptr, buffer, sizeof(buffer))) continue;
        info.address = buffer;
        out.push_back(std::move(info));
    }
    return Result<std::vector<NetworkInterfaceInfo>>::success(std::move(out));
}
} // namespace plpt
