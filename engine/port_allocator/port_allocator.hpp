#pragma once
#include <cstdint>
#include <limits>
#include "../core/types.hpp"
#include "../core/result.hpp"
namespace plpt {
enum class BindScope { Loopback, Lan };
// uintptr_t conserve SOCKET (UINT_PTR) sur Windows x64. Aucun int tronqué.
using NativeSocket = std::uintptr_t;
inline constexpr NativeSocket kInvalidSocket = std::numeric_limits<NativeSocket>::max();
class ReservedListener {
public:
    ReservedListener() = default;
    ~ReservedListener();
    ReservedListener(const ReservedListener&) = delete;
    ReservedListener& operator=(const ReservedListener&) = delete;
    ReservedListener(ReservedListener&&) noexcept;
    ReservedListener& operator=(ReservedListener&&) noexcept;
    bool valid() const { return handle_ != kInvalidSocket; }
    Port port() const { return endpoint_.port; }
    const Endpoint& endpoint() const { return endpoint_; }
    TransportProtocol protocol() const { return protocol_; }
    NativeSocket native_handle() const { return handle_; }
private:
    friend class DynamicPortAllocator;
    ReservedListener(NativeSocket h, Endpoint endpoint, TransportProtocol protocol)
        : handle_(h), endpoint_(std::move(endpoint)), protocol_(protocol) {}
    void reset() noexcept;
    NativeSocket handle_ = kInvalidSocket;
    Endpoint endpoint_;
    TransportProtocol protocol_ = TransportProtocol::Unknown;
};
class DynamicPortAllocator {
public:
    // Prototype de réservation, jamais appelé automatiquement par le moniteur.
    // TCP ici permet de tester l'exclusivité, pas d'activer un fallback produit.
    Result<ReservedListener> acquire(TransportProtocol, IpFamily, BindScope = BindScope::Loopback);
};
} // namespace plpt
