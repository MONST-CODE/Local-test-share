#include "port_allocator.hpp"
#ifdef _WIN32
#include "../platform/windows/winsock.hpp"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <utility>

namespace plpt {
namespace {
#ifdef _WIN32
using RawSocket = SOCKET;
using AddressLength = int;
int last_error() { return WSAGetLastError(); }
void close_socket(RawSocket s) { closesocket(s); }
#else
using RawSocket = int;
using AddressLength = socklen_t;
int last_error() { return errno; }
void close_socket(RawSocket s) { ::close(s); }
#endif
int option(RawSocket s, int level, int name, int value) {
    return setsockopt(s, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}
}
void ReservedListener::reset() noexcept {
    if (valid()) close_socket(static_cast<RawSocket>(handle_));
    handle_ = kInvalidSocket;
    endpoint_ = {};
    protocol_ = TransportProtocol::Unknown;
}
ReservedListener::~ReservedListener() { reset(); }
ReservedListener::ReservedListener(ReservedListener&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidSocket)), endpoint_(std::move(other.endpoint_)),
      protocol_(std::exchange(other.protocol_, TransportProtocol::Unknown)) { other.endpoint_ = {}; }
ReservedListener& ReservedListener::operator=(ReservedListener&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = std::exchange(other.handle_, kInvalidSocket);
        endpoint_ = std::move(other.endpoint_);
        protocol_ = std::exchange(other.protocol_, TransportProtocol::Unknown);
        other.endpoint_ = {};
    }
    return *this;
}
Result<ReservedListener> DynamicPortAllocator::acquire(TransportProtocol proto, IpFamily family, BindScope scope) {
    if ((proto != TransportProtocol::Tcp && proto != TransportProtocol::Udp) ||
        (family != IpFamily::Ipv4 && family != IpFamily::Ipv6) ||
        (scope != BindScope::Loopback && scope != BindScope::Lan))
        return Result<ReservedListener>::failure({ErrorCode::InvalidArgument, "paramètre réseau inconnu", "port_allocator"});
#ifdef _WIN32
    if (const auto code = windows::ensure_winsock(); code != 0)
        return Result<ReservedListener>::failure({ErrorCode::PlatformError, "WSAStartup", "port_allocator", code});
#endif
    const int af = family == IpFamily::Ipv4 ? AF_INET : AF_INET6;
    const int type = proto == TransportProtocol::Tcp ? SOCK_STREAM : SOCK_DGRAM;
    const int protocol = proto == TransportProtocol::Tcp ? IPPROTO_TCP : IPPROTO_UDP;
    Error error{ErrorCode::IoError, "bind(port=0)", "port_allocator"};
    for (int attempt = 0; attempt < 3; ++attempt) {
#ifdef _WIN32
        const auto raw = WSASocketW(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (raw == INVALID_SOCKET)
#else
        const auto raw = ::socket(af, type | SOCK_CLOEXEC, protocol);
        if (raw == -1)
#endif
            return Result<ReservedListener>::failure({ErrorCode::IoError, "socket", "port_allocator", last_error()});
        ReservedListener lease(static_cast<NativeSocket>(raw), {}, proto); // RAII dès la création.
#ifdef _WIN32
        if (option(raw, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1) != 0)
            return Result<ReservedListener>::failure({ErrorCode::IoError, "SO_EXCLUSIVEADDRUSE", "port_allocator", last_error()});
#endif
        // Une réservation IPv6 ne s'empare pas silencieusement d'IPv4.
        if (af == AF_INET6 && option(raw, IPPROTO_IPV6, IPV6_V6ONLY, 1) != 0)
            return Result<ReservedListener>::failure({ErrorCode::IoError, "IPV6_V6ONLY", "port_allocator", last_error()});
        sockaddr_storage address{};
        AddressLength size;
        if (af == AF_INET) {
            auto* a = reinterpret_cast<sockaddr_in*>(&address);
            a->sin_family = AF_INET;
            a->sin_port = 0;
            a->sin_addr.s_addr = htonl(scope == BindScope::Loopback ? INADDR_LOOPBACK : INADDR_ANY);
            size = sizeof(sockaddr_in);
        } else {
            auto* a = reinterpret_cast<sockaddr_in6*>(&address);
            a->sin6_family = AF_INET6;
            a->sin6_port = 0;
            a->sin6_addr = scope == BindScope::Loopback ? in6addr_loopback : in6addr_any;
            size = sizeof(sockaddr_in6);
        }
        if (::bind(raw, reinterpret_cast<const sockaddr*>(&address), size) != 0) {
            error.native_code = last_error();
            continue; // le destructeur ferme le socket avant un nouvel essai port 0.
        }
        sockaddr_storage bound{};
        AddressLength bound_size = sizeof(bound);
        if (getsockname(raw, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0)
            return Result<ReservedListener>::failure({ErrorCode::IoError, "getsockname", "port_allocator", last_error()});
        const auto assigned = af == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port)
                                          : ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
        if (assigned == 0) return Result<ReservedListener>::failure({ErrorCode::Internal, "port nul après bind", "port_allocator"});
        if (proto == TransportProtocol::Tcp && ::listen(raw, SOMAXCONN) != 0)
            return Result<ReservedListener>::failure({ErrorCode::IoError, "listen", "port_allocator", last_error()});
        lease.endpoint_ = {family, family == IpFamily::Ipv4 ? (scope == BindScope::Loopback ? "127.0.0.1" : "0.0.0.0")
            : (scope == BindScope::Loopback ? "::1" : "::"), Port{assigned}};
        return Result<ReservedListener>::success(std::move(lease));
    }
    return Result<ReservedListener>::failure(std::move(error));
}
} // namespace plpt
