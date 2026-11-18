#pragma once
// DevLink — engine/net/transport_socket.hpp
// Détails socket partagés par les deux fabriques de transport (clair et TLS).
// En-tête INTERNE : il n'expose aucun contrat public, seulement des primitives
// système minces et honnêtes (errno/WSAGetLastError remontés tels quels).
#include <cstdint>
#include <string>

#include "../core/result.hpp"

#ifdef _WIN32
#include "../platform/windows/winsock.hpp"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace plpt::net::detail {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

inline int socket_error() {
#ifdef _WIN32
    return static_cast<int>(WSAGetLastError());
#else
    return errno;
#endif
}

inline std::string socket_error_text(int code) {
#ifdef _WIN32
    return "winsock error " + std::to_string(code);
#else
    return std::string("errno ") + std::to_string(code) + " (" + std::strerror(code) + ")";
#endif
}

// Mode non bloquant : contrat v2 des canaux adoptés (wrap_*), utilisé par les
// boucles de poll du plan de contrôle. Echec = faux, errno/WSA conservés.
inline bool set_socket_nonblocking(socket_t fd, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1ul : 0ul;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int wanted = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, wanted) == 0;
#endif
}

// Délais bloquants temporaires (poignée de main sur descripteur adopté) ;
// ms == 0 rend le descripteur à nouveau sans délai.
inline bool set_socket_timeouts_ms(socket_t fd, std::uint32_t ms) {
#ifdef _WIN32
    const DWORD value = ms;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) == 0 &&
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(ms / 1000u);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000u) * 1000u);
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

inline bool would_block_error(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

inline void close_socket(socket_t fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

// Écoute TCP IPv4 sur `bind_address` (port 0 = éphémère, port réel rendu via
// *out_port). Aucune option magique : pas de SO_REUSEADDR, un port encore en
// TIME_WAIT doit échouer visiblement plutôt que se cacher.
inline Result<socket_t> make_tcp_listener(const std::string& bind_address, std::uint16_t* out_port) {
#ifdef _WIN32
    if (ensure_winsock() != 0) {
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, "WSAStartup a echoue", "transport", 0});
    }
#endif
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        const int code = socket_error();
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, socket_error_text(code), "socket", code});
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    if (::inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        close_socket(fd);
        return Result<socket_t>::failure(Error{ErrorCode::InvalidArgument, "adresse de lien invalide : " + bind_address, "transport", 0});
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int code = socket_error();
        close_socket(fd);
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, socket_error_text(code), "bind " + bind_address, code});
    }
    if (::listen(fd, 8) != 0) {
        const int code = socket_error();
        close_socket(fd);
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, socket_error_text(code), "listen", code});
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int bound_len = sizeof(bound);
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        const int code = socket_error();
        close_socket(fd);
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, socket_error_text(code), "getsockname", code});
    }
    if (out_port) {
        *out_port = static_cast<std::uint16_t>(ntohs(bound.sin_port));
    }
    return Result<socket_t>::success(fd);
}

inline Result<socket_t> make_tcp_connection(const std::string& address, std::uint16_t port) {
#ifdef _WIN32
    if (ensure_winsock() != 0) {
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, "WSAStartup a echoue", "transport", 0});
    }
#endif
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        const int code = socket_error();
        return Result<socket_t>::failure(Error{ErrorCode::PlatformError, socket_error_text(code), "socket", code});
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
        close_socket(fd);
        return Result<socket_t>::failure(Error{ErrorCode::InvalidArgument, "adresse de connexion invalide : " + address, "transport", 0});
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
        const int code = socket_error();
        close_socket(fd);
        return Result<socket_t>::failure(Error{ErrorCode::Disconnected, socket_error_text(code), "connect " + address + ":" + std::to_string(port), code});
    }
    return Result<socket_t>::success(fd);
}

inline Result<socket_t> accept_one(socket_t listener) {
    const socket_t fd = ::accept(listener, nullptr, nullptr);
    if (fd == kInvalidSocket) {
        const int code = socket_error();
        return Result<socket_t>::failure(Error{ErrorCode::Disconnected, socket_error_text(code), "accept", code});
    }
    return Result<socket_t>::success(fd);
}

} // namespace plpt::net::detail
