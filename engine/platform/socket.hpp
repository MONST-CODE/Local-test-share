#pragma once
// PLPT — engine/platform/socket.hpp — la seule couche socket du produit.
// Winsock2 (x86_64-w64-windows-gnu) et sockets POSIX (Linux) derrière un type
// RAII unique. Aucune bibliothèque réseau externe, aucune abstraction inutilisée.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "../core/result.hpp"
#include "../core/types.hpp"
#include "platform.hpp"

namespace plpt {

// uintptr_t conserve SOCKET (UINT_PTR) sous Windows x64 sans troncature.
using NativeSocket = std::uintptr_t;
inline constexpr NativeSocket kInvalidSocket = std::numeric_limits<NativeSocket>::max();

// Adresse texte + famille + scope. Le scope distingue les interfaces IPv6
// link-local : deux « fe80::1 » ne sont pas la même destination.
struct IpAddress {
    IpFamily family = IpFamily::Unknown;
    std::string text;
    std::uint32_t scope_id = 0;
    bool valid() const { return (family == IpFamily::Ipv4 || family == IpFamily::Ipv6) && !text.empty(); }
    bool is_any() const { return text == "0.0.0.0" || text == "::"; }
    bool is_loopback() const { return text == "127.0.0.1" || text == "::1"; }
    bool is_multicast() const;
    // 10/8, 172.16/12, 192.168/16, fe80::/10 : le LAN réel, jamais Internet.
    bool is_private_lan() const;
    std::string display(Port port) const;
    static IpAddress any(IpFamily family);
    static IpAddress loopback(IpFamily family);
    static Result<IpAddress> parse(std::string_view text);
};

// Sélection d'interfaces LAN pour l'annonce et la liaison : aucune interface
// « loopback », aucune interface down, IPv4 privé d'abord.
std::vector<std::size_t> select_lan_interfaces(std::span<const NetworkInterfaceInfo> interfaces);

struct PollEntry {
    NativeSocket socket = kInvalidSocket;
    bool want_read = false;
    bool want_write = false;
    // POLLHUP est signalé comme lisible : la dernière lecture donne l'EOF réel
    // au lieu de perdre les octets déjà reçus. Seuls POLLERR/POLLNVAL échouent.
    bool readable = false;
    bool writable = false;
    bool failed = false;
};

// poll(2) / WSAPoll : pas de limite FD_SETSIZE, pas de select().
Result<std::size_t> poll_sockets(std::span<PollEntry> entries, std::chrono::milliseconds timeout);

class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    static Result<Socket> create(TransportProtocol protocol, IpFamily family);
    // Adopte un descripteur déjà ouvert (accept, réservation de port).
    static Socket adopt(NativeSocket handle, TransportProtocol protocol, IpFamily family);

    // POSIX SO_REUSEADDR : plusieurs écouteurs UDP multicast sur le même port.
    Result<void> set_address_reuse(bool enabled);
    // Windows SO_EXCLUSIVEADDRUSE : un port TCP libre n'est pas subtilisable.
    // POSIX : SO_REUSEADDR n'a pas ce danger, l'appel réussit sans effet.
    Result<void> set_exclusive_bind(bool enabled);
    Result<void> set_nonblocking(bool enabled);
    Result<void> set_no_delay(bool enabled);
    Result<void> set_broadcast(bool enabled);
    Result<void> set_ipv6_only(bool enabled);
    Result<void> set_receive_buffer(std::uint32_t bytes);
    Result<void> set_send_buffer(std::uint32_t bytes);
    // Délais bloquants : utilisés par les clients CLI, jamais par les boucles poll.
    Result<void> set_timeouts(std::chrono::milliseconds receive, std::chrono::milliseconds send);

    Result<void> bind(const IpAddress& address, Port port);
    Result<void> listen(int backlog = 64);
    Result<Port> bound_port() const;
    Result<IpAddress> bound_address() const;
    Result<Socket> accept(IpAddress* peer_address, Port* peer_port);
    Result<void> connect(const IpAddress& address, Port port); // initiation non bloquante possible
    Result<int> pending_error() const;   // SO_ERROR après une initiation non bloquante
    Result<bool> peer_reachable() const; // getpeername : la connexion est établie

    Result<void> join_multicast(const IpAddress& group, const IpAddress& interface);
    Result<void> leave_multicast(const IpAddress& group, const IpAddress& interface);
    Result<void> set_multicast_ttl(int ttl);
    Result<void> set_multicast_loop(bool enabled);
    Result<void> set_multicast_interface(const IpAddress& interface);

    // Échec => Result invalide. WouldBlock/TimedOut/Disconnected sont des codes
    // explicites, jamais confondus avec une erreur de programmation.
    Result<std::size_t> send(std::span<const std::uint8_t> data);
    Result<std::size_t> receive(std::span<std::uint8_t> buffer);
    Result<std::size_t> send_to(const IpAddress& address, Port port, std::span<const std::uint8_t> data);
    Result<std::size_t> receive_from(std::span<std::uint8_t> buffer, IpAddress* source, Port* source_port);

    void close();
    bool valid() const { return handle_ != kInvalidSocket; }
    NativeSocket native() const { return handle_; }
    TransportProtocol protocol() const { return protocol_; }
    IpFamily family() const { return family_; }

    static int last_error();
    static bool is_would_block(int code);
    static bool is_interrupted(int code);
    // Traduit errno/WSAGetLastError en ErrorCode produit : un port occupé n'est
    // pas une erreur d'entrée/sortie générique, et l'UI doit pouvoir le dire.
    static Error io_error(const char* operation, const char* context, int code);

private:
    Socket(NativeSocket handle, TransportProtocol protocol, IpFamily family)
        : handle_(handle), protocol_(protocol), family_(family) {}
    Result<void> option(int level, int name, int value, const char* what) const;
    NativeSocket handle_ = kInvalidSocket;
    TransportProtocol protocol_ = TransportProtocol::Unknown;
    IpFamily family_ = IpFamily::Unknown;
};

// Écriture complète bornée dans le temps (clients CLI, tests). Les serveurs
// multiplexés utilisent send() + poll_sockets() et gardent leurs files.
// Initiation + attente de disponibilité + vérification SO_ERROR : la seule façon
// honnête de savoir si une connexion TCP a abouti dans un délai donné.
Result<void> connect_with_timeout(Socket& socket, const IpAddress& address, Port port,
    std::chrono::milliseconds timeout);
Result<void> write_all(Socket& socket, std::span<const std::uint8_t> data, std::chrono::milliseconds timeout);
Result<std::size_t> read_some(Socket& socket, std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout);

// Réveil portable d'une boucle poll : un datagramme loopback, pas de pipe nommé,
// pas d'événement propriétaire. notify() est sûr depuis n'importe quel thread.
class WakeupSignal {
public:
    WakeupSignal() = default;
    ~WakeupSignal() = default;
    WakeupSignal(const WakeupSignal&) = delete;
    WakeupSignal& operator=(const WakeupSignal&) = delete;
    Result<void> open();
    void notify();
    void drain();
    bool valid() const { return socket_.valid(); }
    NativeSocket native() const { return socket_.native(); }
    Socket& socket() { return socket_; }
private:
    Socket socket_;
    IpAddress address_ = IpAddress::loopback(IpFamily::Ipv4);
    Port port_{};
};

} // namespace plpt
