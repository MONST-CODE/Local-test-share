// PLPT — engine/platform/socket.cpp — implémentation native Winsock2 / POSIX.
#include "socket.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include "windows/winsock.hpp"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace plpt {
namespace {
#ifdef _WIN32
using RawSocket = SOCKET;
using AddressLength = int;
using PollHandle = SOCKET;
RawSocket raw(NativeSocket handle) { return static_cast<RawSocket>(handle); }
void destroy(RawSocket socket) { ::closesocket(socket); }
#else
using RawSocket = int;
using AddressLength = socklen_t;
using PollHandle = int;
RawSocket raw(NativeSocket handle) { return static_cast<RawSocket>(handle); }
void destroy(RawSocket socket) { ::close(socket); }
#endif

// Drapeaux d'émission : MSG_NOSIGNAL sur POSIX pour qu'un pair fermé rende
// EPIPE (donc ErrorCode::Disconnected) au lieu de tuer le processus par SIGPIPE.
// Windows n'a pas de SIGPIPE : les flags y sont nuls.
int send_flags() {
#ifdef _WIN32
    return 0;
#elif defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

int family_of(IpFamily family) { return family == IpFamily::Ipv6 ? AF_INET6 : AF_INET; }
int type_of(TransportProtocol protocol) { return protocol == TransportProtocol::Tcp ? SOCK_STREAM : SOCK_DGRAM; }
int protocol_of(TransportProtocol protocol) { return protocol == TransportProtocol::Tcp ? IPPROTO_TCP : IPPROTO_UDP; }

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

struct AddressBytes {
    sockaddr_storage storage{};
    AddressLength length = 0;
};

Result<AddressBytes> encode(const IpAddress& address, Port port) {
    if (!address.valid())
        return Result<AddressBytes>::failure({ErrorCode::InvalidArgument, "adresse invalide", "socket"});
    AddressBytes out;
    if (address.family == IpFamily::Ipv4) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&out.storage);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port.value);
        if (address.text == "0.0.0.0") v4->sin_addr.s_addr = htonl(INADDR_ANY);
        else if (::inet_pton(AF_INET, address.text.c_str(), &v4->sin_addr) != 1)
            return Result<AddressBytes>::failure({ErrorCode::InvalidArgument, "inet_pton IPv4", "socket"});
        out.length = sizeof(sockaddr_in);
    } else {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&out.storage);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port.value);
        v6->sin6_scope_id = address.scope_id;
        if (address.text == "::") v6->sin6_addr = in6addr_any;
        else if (::inet_pton(AF_INET6, address.text.c_str(), &v6->sin6_addr) != 1)
            return Result<AddressBytes>::failure({ErrorCode::InvalidArgument, "inet_pton IPv6", "socket"});
        out.length = sizeof(sockaddr_in6);
    }
    return Result<AddressBytes>::success(out);
}

Result<IpAddress> decode(const sockaddr_storage& storage) {
    std::array<char, INET6_ADDRSTRLEN + 1> text{};
    IpAddress address;
    if (storage.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &v4->sin_addr, text.data(), sizeof(text) - 1) == nullptr)
            return Result<IpAddress>::failure({ErrorCode::PlatformError, "inet_ntop IPv4", "socket", Socket::last_error()});
        address.family = IpFamily::Ipv4;
    } else if (storage.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &v6->sin6_addr, text.data(), sizeof(text) - 1) == nullptr)
            return Result<IpAddress>::failure({ErrorCode::PlatformError, "inet_ntop IPv6", "socket", Socket::last_error()});
        address.family = IpFamily::Ipv6;
        address.scope_id = v6->sin6_scope_id;
    } else {
        return Result<IpAddress>::failure({ErrorCode::PlatformError, "famille d'adresse inconnue", "socket"});
    }
    address.text.assign(text.data());
    return Result<IpAddress>::success(std::move(address));
}

Port port_of(const sockaddr_storage& storage) {
    if (storage.ss_family == AF_INET6) return Port{ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port)};
    return Port{ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port)};
}
} // namespace

bool IpAddress::is_multicast() const {
    if (!valid()) return false;
    if (family == IpFamily::Ipv6) return starts_with(text, "ff");
    unsigned first = 0;
    if (std::sscanf(text.c_str(), "%u.", &first) != 1) return false;
    return first >= 224U && first <= 239U; // 224.0.0.0/4, scope administrable inclus.
}

bool IpAddress::is_private_lan() const {
    if (!valid()) return false;
    if (family == IpFamily::Ipv6) return starts_with(text, "fe80") || starts_with(text, "fd") || starts_with(text, "fc");
    unsigned a = 0, b = 0;
    if (std::sscanf(text.c_str(), "%u.%u.", &a, &b) != 2) return false;
    if (a == 10 || a == 127) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true; // lien local : jamais routé, documenté.
    return false;
}

std::string IpAddress::display(Port port) const {
    if (family == IpFamily::Ipv6) {
        auto out = "[" + text + "]:" + std::to_string(port.value);
        if (scope_id != 0) out += "%25" + std::to_string(scope_id); // %25 : pourcentage encodé.
        return out;
    }
    return text + ":" + std::to_string(port.value);
}

IpAddress IpAddress::any(IpFamily family) {
    IpAddress address;
    address.family = family;
    address.text = family == IpFamily::Ipv6 ? "::" : "0.0.0.0";
    return address;
}

IpAddress IpAddress::loopback(IpFamily family) {
    IpAddress address;
    address.family = family;
    address.text = family == IpFamily::Ipv6 ? "::1" : "127.0.0.1";
    return address;
}

Result<IpAddress> IpAddress::parse(std::string_view text) {
    IpAddress address;
    // « fe80::1%3 » : le scope fait partie de l'identité de l'adresse lien-local.
    auto body = text;
    const auto percent = body.find('%');
    if (percent != std::string_view::npos) {
        const auto scope = body.substr(percent + 1);
        std::uint32_t value = 0;
        if (scope.empty() || std::sscanf(std::string(scope).c_str(), "%u", &value) != 1)
            return Result<IpAddress>::failure({ErrorCode::InvalidArgument, "scope IPv6 invalide", "socket"});
        address.scope_id = value;
        body = body.substr(0, percent);
    }
    if (body.empty() || body.size() > 64)
        return Result<IpAddress>::failure({ErrorCode::InvalidArgument, "adresse vide ou trop longue", "socket"});
    std::array<char, INET6_ADDRSTRLEN + 1> storage{};
    const auto candidate = std::string(body);
    if (::inet_pton(AF_INET, candidate.c_str(), storage.data()) == 1) {
        address.family = IpFamily::Ipv4;
        address.text = candidate;
        return Result<IpAddress>::success(std::move(address));
    }
    if (::inet_pton(AF_INET6, candidate.c_str(), storage.data()) == 1) {
        address.family = IpFamily::Ipv6;
        address.text = candidate;
        return Result<IpAddress>::success(std::move(address));
    }
    return Result<IpAddress>::failure({ErrorCode::InvalidArgument, "ni IPv4 ni IPv6 littérale", "socket"});
}

std::vector<std::size_t> select_lan_interfaces(std::span<const NetworkInterfaceInfo> interfaces) {
    std::vector<std::size_t> chosen;
    auto candidate = [&](const NetworkInterfaceInfo& info) {
        if (!info.is_up || info.is_loopback) return false;
        if (info.family != IpFamily::Ipv4 && info.family != IpFamily::Ipv6) return false;
        auto parsed = IpAddress::parse(info.address);
        return parsed && parsed.value().is_private_lan();
    };
    for (std::size_t index = 0; index < interfaces.size(); ++index) {
        const auto& info = interfaces[index];
        if (info.family == IpFamily::Ipv4 && candidate(info)) chosen.push_back(index);
    }
    for (std::size_t index = 0; index < interfaces.size(); ++index) {
        const auto& info = interfaces[index];
        if (info.family == IpFamily::Ipv6 && candidate(info)) chosen.push_back(index);
    }
    return chosen;
}

int Socket::last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool Socket::is_would_block(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS;
#endif
}

bool Socket::is_interrupted(int code) {
#ifdef _WIN32
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

Error Socket::io_error(const char* operation, const char* context, int code) {
    Error error{ErrorCode::IoError, operation, context, code};
#ifdef _WIN32
    if (code == WSAEWOULDBLOCK) error.code = ErrorCode::WouldBlock;
    else if (code == WSAETIMEDOUT) error.code = ErrorCode::TimedOut;
    else if (code == WSAECONNRESET || code == WSAECONNABORTED || code == WSAESHUTDOWN) error.code = ErrorCode::Disconnected;
    else if (code == WSAEADDRINUSE) error.code = ErrorCode::AlreadyExists;
    else if (code == WSAEACCES) error.code = ErrorCode::PermissionDenied;
    else if (code == WSAENETUNREACH || code == WSAEHOSTUNREACH) error.code = ErrorCode::NotFound;
#else
    if (code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS) error.code = ErrorCode::WouldBlock;
    else if (code == ETIMEDOUT) error.code = ErrorCode::TimedOut;
    else if (code == ECONNRESET || code == EPIPE || code == ECONNABORTED || code == ENOTCONN) error.code = ErrorCode::Disconnected;
    else if (code == EADDRINUSE) error.code = ErrorCode::AlreadyExists;
    else if (code == EACCES || code == EPERM) error.code = ErrorCode::PermissionDenied;
    else if (code == ENETUNREACH || code == EHOSTUNREACH || code == ENODEV) error.code = ErrorCode::NotFound;
#endif
    return error;
}

Socket::Socket(Socket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidSocket)), protocol_(other.protocol_), family_(other.family_) {
    other.protocol_ = TransportProtocol::Unknown;
    other.family_ = IpFamily::Unknown;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalidSocket);
        protocol_ = std::exchange(other.protocol_, TransportProtocol::Unknown);
        family_ = std::exchange(other.family_, IpFamily::Unknown);
    }
    return *this;
}

void Socket::close() {
    if (handle_ == kInvalidSocket) return;
    destroy(raw(handle_));
    handle_ = kInvalidSocket;
    protocol_ = TransportProtocol::Unknown;
    family_ = IpFamily::Unknown;
}

Socket Socket::adopt(NativeSocket handle, TransportProtocol protocol, IpFamily family) {
    return Socket(handle, protocol, family);
}

Result<Socket> Socket::create(TransportProtocol protocol, IpFamily family) {
    if ((protocol != TransportProtocol::Tcp && protocol != TransportProtocol::Udp) ||
        (family != IpFamily::Ipv4 && family != IpFamily::Ipv6))
        return Result<Socket>::failure({ErrorCode::InvalidArgument, "protocole ou famille inconnu", "socket"});
#ifdef _WIN32
    if (const auto startup = windows::ensure_winsock(); startup != 0)
        return Result<Socket>::failure({ErrorCode::PlatformError, "WSAStartup", "socket", startup});
    const auto handle = ::WSASocketW(family_of(family), type_of(protocol), protocol_of(protocol), nullptr, 0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (handle == INVALID_SOCKET)
        return Result<Socket>::failure(io_error("WSASocketW", "socket", last_error()));
#else
    // SOCK_CLOEXEC : aucun descripteur ne fuit vers un processus enfant.
    // WSA_FLAG_NO_HANDLE_INHERIT joue le même rôle sous Windows.
    const auto handle = ::socket(family_of(family), type_of(protocol) | SOCK_CLOEXEC, protocol_of(protocol));
    if (handle < 0) return Result<Socket>::failure(io_error("socket", "socket", last_error()));
#endif
    return Result<Socket>::success(Socket(static_cast<NativeSocket>(handle), protocol, family));
}

Result<void> Socket::option(int level, int name, int value, const char* what) const {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", what});
    if (::setsockopt(raw(handle_), level, name, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
        return Result<void>::failure(io_error("setsockopt", what, last_error()));
    return Result<void>::success();
}

Result<void> Socket::set_address_reuse(bool enabled) { return option(SOL_SOCKET, SO_REUSEADDR, enabled ? 1 : 0, "SO_REUSEADDR"); }

Result<void> Socket::set_exclusive_bind(bool enabled) {
#ifdef _WIN32
    // Sous Windows, SO_REUSEADDR autoriserait un second processus à subtiliser le
    // port. SO_EXCLUSIVEADDRUSE rend « port occupé » détectable et honnête.
    return option(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, enabled ? 1 : 0, "SO_EXCLUSIVEADDRUSE");
#else
    (void)enabled; // POSIX : SO_REUSEADDR n'autorise pas le vol de port TCP.
    return Result<void>::success();
#endif
}

Result<void> Socket::set_nonblocking(bool enabled) {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    if (::ioctlsocket(raw(handle_), FIONBIO, &mode) != 0)
        return Result<void>::failure(io_error("ioctlsocket", "socket", last_error()));
#else
    const auto flags = ::fcntl(raw(handle_), F_GETFL, 0);
    if (flags < 0) return Result<void>::failure(io_error("fcntl(F_GETFL)", "socket", last_error()));
    const auto next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(raw(handle_), F_SETFL, next) != 0)
        return Result<void>::failure(io_error("fcntl(F_SETFL)", "socket", last_error()));
#endif
    return Result<void>::success();
}

Result<void> Socket::set_no_delay(bool enabled) { return option(IPPROTO_TCP, TCP_NODELAY, enabled ? 1 : 0, "TCP_NODELAY"); }
Result<void> Socket::set_broadcast(bool enabled) { return option(SOL_SOCKET, SO_BROADCAST, enabled ? 1 : 0, "SO_BROADCAST"); }
Result<void> Socket::set_ipv6_only(bool enabled) { return option(IPPROTO_IPV6, IPV6_V6ONLY, enabled ? 1 : 0, "IPV6_V6ONLY"); }
Result<void> Socket::set_receive_buffer(std::uint32_t bytes) { return option(SOL_SOCKET, SO_RCVBUF, static_cast<int>(bytes), "SO_RCVBUF"); }
Result<void> Socket::set_send_buffer(std::uint32_t bytes) { return option(SOL_SOCKET, SO_SNDBUF, static_cast<int>(bytes), "SO_SNDBUF"); }

Result<void> Socket::set_timeouts(std::chrono::milliseconds receive, std::chrono::milliseconds send) {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
#ifdef _WIN32
    DWORD receive_ms = static_cast<DWORD>(receive.count());
    DWORD send_ms = static_cast<DWORD>(send.count());
    if (::setsockopt(raw(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_ms), sizeof(receive_ms)) != 0)
        return Result<void>::failure(io_error("setsockopt", "SO_RCVTIMEO", last_error()));
    if (::setsockopt(raw(handle_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_ms), sizeof(send_ms)) != 0)
        return Result<void>::failure(io_error("setsockopt", "SO_SNDTIMEO", last_error()));
#else
    timeval receive_tv{static_cast<time_t>(receive.count() / 1000), static_cast<suseconds_t>((receive.count() % 1000) * 1000)};
    timeval send_tv{static_cast<time_t>(send.count() / 1000), static_cast<suseconds_t>((send.count() % 1000) * 1000)};
    if (::setsockopt(raw(handle_), SOL_SOCKET, SO_RCVTIMEO, &receive_tv, sizeof(receive_tv)) != 0)
        return Result<void>::failure(io_error("setsockopt", "SO_RCVTIMEO", last_error()));
    if (::setsockopt(raw(handle_), SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv)) != 0)
        return Result<void>::failure(io_error("setsockopt", "SO_SNDTIMEO", last_error()));
#endif
    return Result<void>::success();
}

Result<void> Socket::bind(const IpAddress& address, Port port) {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    auto bytes = encode(address, port);
    if (!bytes) return Result<void>::failure(bytes.error());
    const auto& value = bytes.value();
    if (::bind(raw(handle_), reinterpret_cast<const sockaddr*>(&value.storage), value.length) != 0)
        return Result<void>::failure(io_error("bind", "socket", last_error()));
    return Result<void>::success();
}

Result<void> Socket::listen(int backlog) {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    if (protocol_ != TransportProtocol::Tcp)
        return Result<void>::failure({ErrorCode::InvalidArgument, "listen exige TCP", "socket"});
    if (::listen(raw(handle_), backlog) != 0) return Result<void>::failure(io_error("listen", "socket", last_error()));
    return Result<void>::success();
}

Result<Port> Socket::bound_port() const {
    if (!valid()) return Result<Port>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    sockaddr_storage storage{};
    AddressLength length = sizeof(storage);
    if (::getsockname(raw(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0)
        return Result<Port>::failure(io_error("getsockname", "socket", last_error()));
    return Result<Port>::success(port_of(storage));
}

Result<IpAddress> Socket::bound_address() const {
    if (!valid()) return Result<IpAddress>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    sockaddr_storage storage{};
    AddressLength length = sizeof(storage);
    if (::getsockname(raw(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0)
        return Result<IpAddress>::failure(io_error("getsockname", "socket", last_error()));
    return decode(storage);
}

Result<Socket> Socket::accept(IpAddress* peer_address, Port* peer_port) {
    if (!valid()) return Result<Socket>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    sockaddr_storage storage{};
    AddressLength length = sizeof(storage);
    const auto client = ::accept(raw(handle_), reinterpret_cast<sockaddr*>(&storage), &length);
#ifdef _WIN32
    if (client == INVALID_SOCKET) {
        const auto code = last_error();
        return Result<Socket>::failure(io_error(is_would_block(code) ? "accept(pending)" : "accept", "socket", code));
    }
    // WSASocketW n'hérite pas ; accept() si : fermeture explicite à la sortie.
    ::SetHandleInformation(reinterpret_cast<HANDLE>(client), HANDLE_FLAG_INHERIT, 0);
#else
    if (client < 0) {
        const auto code = last_error();
        return Result<Socket>::failure(io_error(is_would_block(code) ? "accept(pending)" : "accept", "socket", code));
    }
#endif
    if (length > 0) {
        if (auto decoded = decode(storage); decoded) {
            if (peer_address) *peer_address = decoded.value();
            if (peer_port) *peer_port = port_of(storage);
        }
    }
    auto accepted = Socket(static_cast<NativeSocket>(client), protocol_, family_);
    if (auto blocking = accepted.set_nonblocking(false); !blocking) return Result<Socket>::failure(blocking.error());
    return Result<Socket>::success(std::move(accepted));
}

Result<void> Socket::connect(const IpAddress& address, Port port) {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    auto bytes = encode(address, port);
    if (!bytes) return Result<void>::failure(bytes.error());
    const auto& value = bytes.value();
    if (::connect(raw(handle_), reinterpret_cast<const sockaddr*>(&value.storage), value.length) == 0) return Result<void>::success();
    const auto code = last_error();
    // Non bloquant : la connexion est en cours, le résultat vient de SO_ERROR.
    if (is_would_block(code)) return Result<void>::success();
    if (is_interrupted(code)) return Result<void>::failure(io_error("connect(interrupt)", "socket", code));
    return Result<void>::failure(io_error("connect", "socket", code));
}

Result<int> Socket::pending_error() const {
    if (!valid()) return Result<int>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    int stored = 0;
    AddressLength length = sizeof(stored);
    if (::getsockopt(raw(handle_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&stored), &length) != 0)
        return Result<int>::failure(io_error("getsockopt(SO_ERROR)", "socket", last_error()));
    return Result<int>::success(stored);
}

Result<bool> Socket::peer_reachable() const {
    if (!valid()) return Result<bool>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    sockaddr_storage storage{};
    AddressLength length = sizeof(storage);
    if (::getpeername(raw(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        const auto code = last_error();
#ifdef _WIN32
        if (code == WSAENOTCONN) return Result<bool>::success(false);
#else
        if (code == ENOTCONN) return Result<bool>::success(false);
#endif
        return Result<bool>::failure(io_error("getpeername", "socket", code));
    }
    return Result<bool>::success(true);
}

Result<void> Socket::join_multicast(const IpAddress& group, const IpAddress& interface) {
    if (!valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    if (!group.is_multicast())
        return Result<void>::failure({ErrorCode::InvalidArgument, "groupe non multicast", "socket"});
    if (group.family != family_)
        return Result<void>::failure({ErrorCode::InvalidArgument, "famille de groupe incohérente", "socket"});
    if (family_ == IpFamily::Ipv4) {
        ip_mreq request{};
        if (::inet_pton(AF_INET, group.text.c_str(), &request.imr_multiaddr) != 1)
            return Result<void>::failure({ErrorCode::InvalidArgument, "groupe IPv4 invalide", "socket"});
        if (interface.valid() && interface.family == IpFamily::Ipv4) {
            if (::inet_pton(AF_INET, interface.text.c_str(), &request.imr_interface) != 1)
                return Result<void>::failure({ErrorCode::InvalidArgument, "interface IPv4 invalide", "socket"});
        } else {
            request.imr_interface.s_addr = htonl(INADDR_ANY);
        }
        if (::setsockopt(raw(handle_), IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&request), sizeof(request)) != 0)
            return Result<void>::failure(io_error("IP_ADD_MEMBERSHIP", "socket", last_error()));
        return Result<void>::success();
    }
    ipv6_mreq request{};
    if (::inet_pton(AF_INET6, group.text.c_str(), &request.ipv6mr_multiaddr) != 1)
        return Result<void>::failure({ErrorCode::InvalidArgument, "groupe IPv6 invalide", "socket"});
    request.ipv6mr_interface = interface.scope_id;
    if (::setsockopt(raw(handle_), IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char*>(&request), sizeof(request)) != 0)
        return Result<void>::failure(io_error("IPV6_JOIN_GROUP", "socket", last_error()));
    return Result<void>::success();
}

Result<void> Socket::leave_multicast(const IpAddress& group, const IpAddress& interface) {
    if (!valid()) return Result<void>::success();
    if (family_ == IpFamily::Ipv4) {
        ip_mreq request{};
        if (::inet_pton(AF_INET, group.text.c_str(), &request.imr_multiaddr) != 1) return Result<void>::success();
        request.imr_interface.s_addr = htonl(INADDR_ANY);
        if (interface.valid() && interface.family == IpFamily::Ipv4)
            (void)::inet_pton(AF_INET, interface.text.c_str(), &request.imr_interface);
        (void)::setsockopt(raw(handle_), IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&request), sizeof(request));
        return Result<void>::success();
    }
    ipv6_mreq request{};
    if (::inet_pton(AF_INET6, group.text.c_str(), &request.ipv6mr_multiaddr) != 1) return Result<void>::success();
    request.ipv6mr_interface = interface.scope_id;
    (void)::setsockopt(raw(handle_), IPPROTO_IPV6, IPV6_LEAVE_GROUP, reinterpret_cast<const char*>(&request), sizeof(request));
    return Result<void>::success();
}

Result<void> Socket::set_multicast_ttl(int ttl) {
    if (ttl < 1 || ttl > 64) return Result<void>::failure({ErrorCode::InvalidArgument, "TTL multicast hors 1..64", "socket"});
    if (family_ == IpFamily::Ipv6) {
        int hops = ttl;
        if (::setsockopt(raw(handle_), IPPROTO_IPV6, IPV6_MULTICAST_HOPS, reinterpret_cast<const char*>(&hops), sizeof(hops)) != 0)
            return Result<void>::failure(io_error("IPV6_MULTICAST_HOPS", "socket", last_error()));
        return Result<void>::success();
    }
#ifdef _WIN32
    // Winsock exige un DWORD pour IP_MULTICAST_TTL, contrairement à POSIX (u_char).
    DWORD value = static_cast<DWORD>(ttl);
    if (::setsockopt(raw(handle_), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
        return Result<void>::failure(io_error("IP_MULTICAST_TTL", "socket", last_error()));
    return Result<void>::success();
#else
    unsigned char value = static_cast<unsigned char>(ttl);
    if (::setsockopt(raw(handle_), IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value)) != 0)
        return Result<void>::failure(io_error("IP_MULTICAST_TTL", "socket", last_error()));
    return Result<void>::success();
#endif
}

Result<void> Socket::set_multicast_loop(bool enabled) {
    if (family_ == IpFamily::Ipv6) return option(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, enabled ? 1 : 0, "IPV6_MULTICAST_LOOP");
#ifdef _WIN32
    DWORD value = enabled ? 1UL : 0UL;
    if (::setsockopt(raw(handle_), IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
        return Result<void>::failure(io_error("IP_MULTICAST_LOOP", "socket", last_error()));
    return Result<void>::success();
#else
    unsigned char value = enabled ? 1U : 0U;
    if (::setsockopt(raw(handle_), IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value)) != 0)
        return Result<void>::failure(io_error("IP_MULTICAST_LOOP", "socket", last_error()));
    return Result<void>::success();
#endif
}

Result<void> Socket::set_multicast_interface(const IpAddress& interface) {
    if (!interface.valid()) return Result<void>::failure({ErrorCode::InvalidArgument, "interface invalide", "socket"});
    if (family_ == IpFamily::Ipv6) {
        if (interface.scope_id == 0)
            return Result<void>::failure({ErrorCode::InvalidArgument, "IPv6 exige un scope d'interface", "socket"});
        // Index d'interface sur 4 octets : DWORD sous Windows, unsigned int sous POSIX.
        const std::uint32_t index = interface.scope_id;
        if (::setsockopt(raw(handle_), IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<const char*>(&index), sizeof(index)) != 0)
            return Result<void>::failure(io_error("IPV6_MULTICAST_IF", "socket", last_error()));
        return Result<void>::success();
    }
    in_addr local{};
    if (::inet_pton(AF_INET, interface.text.c_str(), &local) != 1)
        return Result<void>::failure({ErrorCode::InvalidArgument, "interface IPv4 invalide", "socket"});
    if (::setsockopt(raw(handle_), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local)) != 0)
        return Result<void>::failure(io_error("IP_MULTICAST_IF", "socket", last_error()));
    return Result<void>::success();
}

Result<std::size_t> Socket::send(std::span<const std::uint8_t> data) {
    if (!valid()) return Result<std::size_t>::failure({ErrorCode::Disconnected, "socket fermé", "socket"});
    if (data.empty()) return Result<std::size_t>::success(0);
    const auto limit = static_cast<int>(std::min<std::size_t>(data.size(), 1u << 20));
    // MSG_NOSIGNAL est indispensable : sans lui, écrire sur un pair fermé lève
    // SIGPIPE et tue tout le processus au lieu de rendre EPIPE/Disconnected.
    const auto sent = ::send(raw(handle_), reinterpret_cast<const char*>(data.data()), limit, send_flags());
#ifdef _WIN32
    if (sent == SOCKET_ERROR)
#else
    if (sent < 0)
#endif
    {
        const auto code = last_error();
        if (is_interrupted(code)) return Result<std::size_t>::success(0);
        return Result<std::size_t>::failure(io_error("send", "socket", code));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(sent));
}

Result<std::size_t> Socket::receive(std::span<std::uint8_t> buffer) {
    if (!valid()) return Result<std::size_t>::failure({ErrorCode::Disconnected, "socket fermé", "socket"});
    if (buffer.empty()) return Result<std::size_t>::success(0);
    const auto limit = static_cast<int>(std::min<std::size_t>(buffer.size(), 1u << 20));
    const auto got = ::recv(raw(handle_), reinterpret_cast<char*>(buffer.data()), limit, 0);
#ifdef _WIN32
    if (got == SOCKET_ERROR) {
        const auto code = last_error();
        if (is_interrupted(code)) return Result<std::size_t>::success(0);
        return Result<std::size_t>::failure(io_error("recv", "socket", code));
    }
#else
    if (got < 0) {
        const auto code = last_error();
        if (is_interrupted(code)) return Result<std::size_t>::success(0);
        return Result<std::size_t>::failure(io_error("recv", "socket", code));
    }
#endif
    if (got == 0 && protocol_ == TransportProtocol::Tcp)
        return Result<std::size_t>::failure({ErrorCode::Disconnected, "lecture TCP à zéro : pair fermé", "socket"});
    return Result<std::size_t>::success(static_cast<std::size_t>(got));
}

Result<std::size_t> Socket::send_to(const IpAddress& address, Port port, std::span<const std::uint8_t> data) {
    if (!valid()) return Result<std::size_t>::failure({ErrorCode::Disconnected, "socket fermé", "socket"});
    auto bytes = encode(address, port);
    if (!bytes) return Result<std::size_t>::failure(bytes.error());
    const auto& value = bytes.value();
    const auto limit = static_cast<int>(std::min<std::size_t>(data.size(), 65507));
    const auto sent = ::sendto(raw(handle_), reinterpret_cast<const char*>(data.data()), data.empty() ? 0 : limit, send_flags(),
        reinterpret_cast<const sockaddr*>(&value.storage), value.length);
#ifdef _WIN32
    if (sent == SOCKET_ERROR)
#else
    if (sent < 0)
#endif
    {
        const auto code = last_error();
        if (is_interrupted(code)) return Result<std::size_t>::success(0);
        return Result<std::size_t>::failure(io_error("sendto", "socket", code));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(sent));
}

Result<std::size_t> Socket::receive_from(std::span<std::uint8_t> buffer, IpAddress* source, Port* source_port) {
    if (!valid()) return Result<std::size_t>::failure({ErrorCode::Disconnected, "socket fermé", "socket"});
    sockaddr_storage storage{};
    AddressLength length = sizeof(storage);
    const auto got = ::recvfrom(raw(handle_), reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(std::min<std::size_t>(buffer.size(), 65535)), 0, reinterpret_cast<sockaddr*>(&storage), &length);
#ifdef _WIN32
    if (got == SOCKET_ERROR)
#else
    if (got < 0)
#endif
    {
        const auto code = last_error();
        if (is_interrupted(code)) return Result<std::size_t>::success(0);
        return Result<std::size_t>::failure(io_error("recvfrom", "socket", code));
    }
    if (length > 0) {
        if (auto decoded = decode(storage); decoded) {
            if (source) *source = decoded.value();
            if (source_port) *source_port = port_of(storage);
        }
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(got));
}

Result<std::size_t> poll_sockets(std::span<PollEntry> entries, std::chrono::milliseconds timeout) {
    if (entries.empty()) return Result<std::size_t>::success(0);
    std::vector<pollfd> descriptors(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& entry = entries[index];
        entry.readable = false;
        entry.writable = false;
        entry.failed = entry.socket == kInvalidSocket;
        if (entry.failed) { // un descripteur invalide ne doit jamais casser l'attente.
            descriptors[index].fd = static_cast<PollHandle>(raw(kInvalidSocket));
            descriptors[index].events = 0;
            descriptors[index].revents = 0;
            continue;
        }
        descriptors[index].fd = static_cast<PollHandle>(raw(entry.socket));
        short events = 0; // NOLINT(google-runtime-int) : poll exige short.
        if (entry.want_read) events |= POLLRDNORM;
        if (entry.want_write) events |= POLLWRNORM;
        descriptors[index].events = events;
        descriptors[index].revents = 0;
    }
    const auto wait_ms = static_cast<int>(std::clamp<long long>(timeout.count(), 0, 60000));
    int ready = 0;
#ifdef _WIN32
    ready = ::WSAPoll(descriptors.data(), static_cast<ULONG>(descriptors.size()), wait_ms);
    if (ready == SOCKET_ERROR) return Result<std::size_t>::failure(Socket::io_error("WSAPoll", "socket", Socket::last_error()));
#else
    do { ready = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), wait_ms); }
    while (ready < 0 && Socket::is_interrupted(errno));
    if (ready < 0) return Result<std::size_t>::failure(Socket::io_error("poll", "socket", Socket::last_error()));
#endif
    std::size_t count = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto events = descriptors[index].revents;
        auto& entry = entries[index];
        if (events == 0 && !entry.failed) continue;
        ++count;
        if (events & (POLLERR | POLLNVAL)) entry.failed = true;
        if (events & (POLLRDNORM | POLLHUP)) entry.readable = true;
        if (events & POLLWRNORM) entry.writable = true;
    }
    return Result<std::size_t>::success(count);
}

Result<void> connect_with_timeout(Socket& socket, const IpAddress& address, Port port, std::chrono::milliseconds timeout) {
    if (!socket.valid()) return Result<void>::failure({ErrorCode::InvalidState, "socket fermé", "socket"});
    if (auto initiated = socket.connect(address, port); !initiated) return initiated;
    PollEntry entry{socket.native(), false, true, false, false, false};
    auto waited = poll_sockets({&entry, 1}, timeout);
    if (!waited) return Result<void>::failure(waited.error());
    if (waited.value() == 0) return Result<void>::failure({ErrorCode::TimedOut, "connexion TCP non établie dans le délai", "socket"});
    if (entry.failed) {
        auto pending = socket.pending_error();
        const int code = pending ? pending.value() : 0;
        return Result<void>::failure(Socket::io_error("connect", "socket", code == 0 ? 1 : code));
    }
    auto pending = socket.pending_error();
    if (!pending) return Result<void>::failure(pending.error());
    if (pending.value() != 0) return Result<void>::failure(Socket::io_error("connect", "socket", pending.value()));
    auto reachable = socket.peer_reachable();
    if (!reachable) return Result<void>::failure(reachable.error());
    if (!reachable.value()) return Result<void>::failure({ErrorCode::Disconnected, "connexion refusée ou refermée", "socket"});
    return Result<void>::success();
}

Result<void> write_all(Socket& socket, std::span<const std::uint8_t> data, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto sent = socket.send(data.subspan(offset));
        if (sent) { offset += sent.value(); continue; }
        if (sent.error().code != ErrorCode::WouldBlock) return Result<void>::failure(sent.error());
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return Result<void>::failure({ErrorCode::TimedOut, "écriture incomplète dans le délai", "socket"});
        PollEntry entry{socket.native(), false, true, false, false, false};
        auto waited = poll_sockets({&entry, 1}, remaining);
        if (!waited) return Result<void>::failure(waited.error());
        if (waited.value() == 0) return Result<void>::failure({ErrorCode::TimedOut, "écriture incomplète dans le délai", "socket"});
        if (entry.failed) return Result<void>::failure({ErrorCode::Disconnected, "écriture impossible", "socket"});
    }
    return Result<void>::success();
}

Result<std::size_t> read_some(Socket& socket, std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto got = socket.receive(buffer);
        if (got) return got;
        if (got.error().code != ErrorCode::WouldBlock) return Result<std::size_t>::failure(got.error());
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return Result<std::size_t>::failure({ErrorCode::TimedOut, "aucune donnée dans le délai", "socket"});
        PollEntry entry{socket.native(), true, false, false, false, false};
        auto waited = poll_sockets({&entry, 1}, remaining);
        if (!waited) return Result<std::size_t>::failure(waited.error());
        if (waited.value() == 0) return Result<std::size_t>::failure({ErrorCode::TimedOut, "aucune donnée dans le délai", "socket"});
        if (entry.failed) return Result<std::size_t>::failure({ErrorCode::Disconnected, "lecture impossible", "socket"});
    }
}

Result<void> WakeupSignal::open() {
    auto created = Socket::create(TransportProtocol::Udp, IpFamily::Ipv4);
    if (!created) return Result<void>::failure(created.error());
    socket_ = std::move(created).value();
    if (auto bound = socket_.bind(IpAddress::loopback(IpFamily::Ipv4), Port{0}); !bound) return bound;
    auto port = socket_.bound_port();
    if (!port) return Result<void>::failure(port.error());
    port_ = port.value();
    if (auto nonblocking = socket_.set_nonblocking(true); !nonblocking) return nonblocking;
    return Result<void>::success();
}

void WakeupSignal::notify() {
    if (!socket_.valid()) return;
    const std::uint8_t byte = 1;
    (void)socket_.send_to(address_, port_, {&byte, 1});
}

void WakeupSignal::drain() {
    if (!socket_.valid()) return;
    std::array<std::uint8_t, 512> buffer{};
    for (int attempt = 0; attempt < 64; ++attempt) {
        auto got = socket_.receive_from(buffer, nullptr, nullptr);
        if (!got || got.value() == 0) return;
    }
}

} // namespace plpt
