#include "winsock.hpp"
#include "windows_platform.hpp"
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <cstddef>
#include <limits>
#include <string_view>

namespace plpt {
namespace {
std::string utf8(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const auto n = static_cast<int>(text.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), n, nullptr, 0, nullptr, nullptr);
    if (count == 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), n, out.data(), count, nullptr, nullptr) != count) return {};
    return out;
}
std::string address(int family, const void* ptr) {
    char text[INET6_ADDRSTRLEN]{};
    if (!InetNtopA(family, ptr, text, sizeof(text))) return {};
    return text;
}
SocketState tcp_state(DWORD state) {
    switch (state) {
    case MIB_TCP_STATE_LISTEN: return SocketState::Listening;
    case MIB_TCP_STATE_ESTAB: return SocketState::Established;
    case MIB_TCP_STATE_TIME_WAIT: return SocketState::TimeWait;
    case MIB_TCP_STATE_CLOSED: return SocketState::Closed;
    default: return SocketState::Connected; // FIN/SYN/etc. ne sont PAS des listeners.
    }
}
Port port(DWORD network_order) { return Port{ntohs(static_cast<u_short>(network_order))}; }

// Table de taille variable : croissance entre les deux appels tolérée et bornée.
// Chaque famille utilise son VRAI layout MIB_*6TABLE_OWNER_PID, jamais un cast IPv4.
template <typename Table, typename Getter, typename Convert>
void append_table(Observation<SocketRecord>& out, const char* operation, Getter get, Convert convert) {
    ULONG bytes = 0;
    DWORD code = get(nullptr, &bytes);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (code != ERROR_INSUFFICIENT_BUFFER && code != NO_ERROR) break;
        if (bytes < sizeof(DWORD) || bytes > 16U * 1024U * 1024U) { code = ERROR_INVALID_DATA; break; }
        // vector allocation has fundamental alignment; no packed/IPv4 reinterpretation.
        std::vector<std::uint8_t> buffer(bytes);
        code = get(buffer.data(), &bytes);
        if (code == ERROR_INSUFFICIENT_BUFFER) continue;
        if (code != NO_ERROR) break;
        const auto* table = reinterpret_cast<const Table*>(buffer.data());
        constexpr auto offset = offsetof(Table, table);
        using Row = decltype(table->table[0]);
        if (bytes > buffer.size() || bytes < offset ||
            table->dwNumEntries > (bytes - offset) / sizeof(Row)) {
            code = ERROR_INVALID_DATA;
            break;
        }
        for (DWORD i = 0; i < table->dwNumEntries; ++i) out.records.push_back(convert(table->table[i]));
        return;
    }
    out.complete = false;
    out.warnings.push_back({ErrorCode::PlatformError, "table indisponible ou instable", operation, code});
}
} // namespace

Result<Observation<SocketRecord>> WindowsSocketTableProvider::snapshot() {
    if (const auto code = windows::ensure_winsock(); code != 0)
        return Result<Observation<SocketRecord>>::failure({ErrorCode::PlatformError, "WSAStartup", "sockets", code});
    Observation<SocketRecord> out;
    append_table<MIB_TCPTABLE_OWNER_PID>(out, "TCP/IPv4", [](void* b, ULONG* n) {
        return GetExtendedTcpTable(b, n, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    }, [](const MIB_TCPROW_OWNER_PID& r) {
        return SocketRecord{TransportProtocol::Tcp,
            {IpFamily::Ipv4, address(AF_INET, &r.dwLocalAddr), port(r.dwLocalPort)},
            {IpFamily::Ipv4, address(AF_INET, &r.dwRemoteAddr), port(r.dwRemotePort)},
            tcp_state(r.dwState), ProcessId{r.dwOwningPid}};
    });
    append_table<MIB_TCP6TABLE_OWNER_PID>(out, "TCP/IPv6", [](void* b, ULONG* n) {
        return GetExtendedTcpTable(b, n, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    }, [](const MIB_TCP6ROW_OWNER_PID& r) {
        return SocketRecord{TransportProtocol::Tcp,
            {IpFamily::Ipv6, address(AF_INET6, r.ucLocalAddr), port(r.dwLocalPort), r.dwLocalScopeId},
            {IpFamily::Ipv6, address(AF_INET6, r.ucRemoteAddr), port(r.dwRemotePort), r.dwRemoteScopeId},
            tcp_state(r.dwState), ProcessId{r.dwOwningPid}};
    });
    append_table<MIB_UDPTABLE_OWNER_PID>(out, "UDP/IPv4", [](void* b, ULONG* n) {
        return GetExtendedUdpTable(b, n, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    }, [](const MIB_UDPROW_OWNER_PID& r) {
        return SocketRecord{TransportProtocol::Udp,
            {IpFamily::Ipv4, address(AF_INET, &r.dwLocalAddr), port(r.dwLocalPort)}, {},
            SocketState::Bound, ProcessId{r.dwOwningPid}};
    });
    append_table<MIB_UDP6TABLE_OWNER_PID>(out, "UDP/IPv6", [](void* b, ULONG* n) {
        return GetExtendedUdpTable(b, n, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    }, [](const MIB_UDP6ROW_OWNER_PID& r) {
        return SocketRecord{TransportProtocol::Udp,
            {IpFamily::Ipv6, address(AF_INET6, r.ucLocalAddr), port(r.dwLocalPort), r.dwLocalScopeId}, {},
            SocketState::Bound, ProcessId{r.dwOwningPid}};
    });
    return Result<Observation<SocketRecord>>::success(std::move(out));
}

Result<Observation<ProcessRecord>> WindowsProcessEnumerator::enumerate() {
    windows::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return Result<Observation<ProcessRecord>>::failure(
        {ErrorCode::PlatformError, "CreateToolhelp32Snapshot", "processes", GetLastError()});
    Observation<ProcessRecord> out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL available = Process32FirstW(snapshot.get(), &entry);
    while (available) {
        ProcessRecord r;
        r.pid = ProcessId{entry.th32ProcessID};
        r.ppid = ProcessId{entry.th32ParentProcessID}; // champ documenté, PAS NtQueryInformationProcess.
        r.image_name = utf8(entry.szExeFile);
        windows::UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (process) {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (GetProcessTimes(process.get(), &creation, &exit, &kernel, &user))
                r.start_time_ticks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
            std::wstring path(32768, L'\0');
            DWORD size = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) {
                path.resize(size);
                const auto converted = utf8(path);
                if (!converted.empty()) r.executable_path = converted;
            }
        } // refus d'accès : champs optionnels absents, aucune élévation.
        if (r.pid.value != 0) out.records.push_back(std::move(r));
        available = Process32NextW(snapshot.get(), &entry);
    }
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
        out.complete = false;
        out.warnings.push_back({ErrorCode::PlatformError, "énumération interrompue", "processes", error});
    }
    return Result<Observation<ProcessRecord>>::success(std::move(out));
}

Result<std::vector<NetworkInterfaceInfo>> WindowsNetworkInterfaceProvider::enumerate() {
    if (const int code = windows::ensure_winsock(); code != 0)
        return Result<std::vector<NetworkInterfaceInfo>>::failure({ErrorCode::PlatformError, "WSAStartup", "interfaces", code});
    ULONG size = 16384;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (size > 16U * 1024U * 1024U) break;
        std::vector<std::uint8_t> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
        if (code == ERROR_BUFFER_OVERFLOW) continue;
        if (code == ERROR_NO_DATA) return Result<std::vector<NetworkInterfaceInfo>>::success({});
        if (code != NO_ERROR) return Result<std::vector<NetworkInterfaceInfo>>::failure(
            {ErrorCode::PlatformError, "GetAdaptersAddresses", "interfaces", code});
        std::vector<NetworkInterfaceInfo> out;
        for (auto* a = adapters; a; a = a->Next) {
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr) continue;
                const int fam = u->Address.lpSockaddr->sa_family;
                if (fam != AF_INET && fam != AF_INET6) continue;
                NetworkInterfaceInfo info;
                info.name = a->FriendlyName ? utf8(a->FriendlyName) : std::string{};
                info.family = fam == AF_INET ? IpFamily::Ipv4 : IpFamily::Ipv6;
                info.is_up = a->OperStatus == IfOperStatusUp;
                info.is_loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
                if (fam == AF_INET) {
                    info.address = address(fam, &reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr);
                } else {
                    const auto* addr = reinterpret_cast<const sockaddr_in6*>(u->Address.lpSockaddr);
                    info.address = address(fam, &addr->sin6_addr);
                    info.scope_id = addr->sin6_scope_id;
                }
                out.push_back(std::move(info));
            }
        }
        return Result<std::vector<NetworkInterfaceInfo>>::success(std::move(out));
    }
    return Result<std::vector<NetworkInterfaceInfo>>::failure(
        {ErrorCode::PlatformError, "table trop grande ou instable", "interfaces"});
}
} // namespace plpt
