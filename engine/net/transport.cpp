// DevLink — engine/net/transport.cpp
// Fabrique TCP CLAIR derrière l'interface de transport versionnée.
// Ce transport n'est pas un échec caché : il se déclare clair partout
// (TransportInfo.protocol = "tcp clair", peer_authenticated = false) et le plan
// de contrôle production refusera de l'utiliser pour un partage réel (étape 5).
// Il sert aux diagnostics locaux, aux tests de comparaison et au constat
// honnête « TLS non intégré » tant que la fabrique TLS n'est pas branchée.
#include "transport.hpp"
#include "transport_socket.hpp"

#include <utility>

namespace plpt::net {
namespace {

class ClearTcpChannel final : public ITransportChannel {
public:
    explicit ClearTcpChannel(detail::socket_t fd) : fd_(fd) {}
    ~ClearTcpChannel() override { close(); }

    Result<std::size_t> send(std::span<const std::uint8_t> data) override {
        if (fd_ == detail::kInvalidSocket) {
            return Result<std::size_t>::failure(Error{ErrorCode::Disconnected, "canal clair ferme", "send", 0});
        }
        std::size_t total = 0;
        while (total < data.size()) {
            const auto sent = ::send(fd_, reinterpret_cast<const char*>(data.data()) + total,
                static_cast<int>(data.size() - total), 0);
            if (sent <= 0) {
                const int code = detail::socket_error();
                return Result<std::size_t>::failure(Error{ErrorCode::IoError, detail::socket_error_text(code), "send clair", code});
            }
            total += static_cast<std::size_t>(sent);
        }
        return Result<std::size_t>::success(total);
    }

    Result<std::size_t> receive(std::span<std::uint8_t> out) override {
        if (fd_ == detail::kInvalidSocket) {
            return Result<std::size_t>::failure(Error{ErrorCode::Disconnected, "canal clair ferme", "receive", 0});
        }
        const auto got = ::recv(fd_, reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()), 0);
        if (got == 0) {
            return Result<std::size_t>::failure(Error{ErrorCode::Disconnected, "pair clair deconnecte", "receive", 0});
        }
        if (got < 0) {
            const int code = detail::socket_error();
            return Result<std::size_t>::failure(Error{ErrorCode::IoError, detail::socket_error_text(code), "receive clair", code});
        }
        return Result<std::size_t>::success(static_cast<std::size_t>(got));
    }

    void close() override {
        if (fd_ != detail::kInvalidSocket) {
            detail::close_socket(fd_);
            fd_ = detail::kInvalidSocket;
        }
    }

    TransportInfo info() const override {
        TransportInfo info;
        info.kind = TransportKind::ClearTcp;
        info.interface_version = kTransportInterfaceVersion;
        info.protocol = "tcp clair";
        info.cipher.clear();
        info.peer_authenticated = false; // jamais d'authentification pretendue
        return info;
    }

private:
    detail::socket_t fd_;
};

class ClearTcpEndpoint final : public ITransportEndpoint {
public:
    ClearTcpEndpoint(detail::socket_t fd, std::uint16_t port) : fd_(fd), port_(port) {}
    ~ClearTcpEndpoint() override { close(); }

    Result<std::unique_ptr<ITransportChannel>> accept() override {
        auto accepted = detail::accept_one(fd_);
        if (!accepted) return Result<std::unique_ptr<ITransportChannel>>::failure(accepted.error());
        return Result<std::unique_ptr<ITransportChannel>>::success(
            std::make_unique<ClearTcpChannel>(accepted.value()));
    }

    std::uint16_t port() const override { return port_; }

    void close() override {
        if (fd_ != detail::kInvalidSocket) {
            detail::close_socket(fd_);
            fd_ = detail::kInvalidSocket;
        }
    }

private:
    detail::socket_t fd_;
    std::uint16_t port_;
};

class ClearTcpFactory final : public ITransportFactory {
public:
    std::uint32_t interface_version() const override { return kTransportInterfaceVersion; }
    TransportKind kind() const override { return TransportKind::ClearTcp; }
    bool available() const override { return true; }
    std::string_view unavailability_reason() const override { return {}; }

    Result<std::unique_ptr<ITransportEndpoint>> listen(
        const std::string& bind_address, const TransportCredentials&) override {
        std::uint16_t port = 0;
        auto listener = detail::make_tcp_listener(bind_address, &port);
        if (!listener) return Result<std::unique_ptr<ITransportEndpoint>>::failure(listener.error());
        return Result<std::unique_ptr<ITransportEndpoint>>::success(
            std::make_unique<ClearTcpEndpoint>(listener.value(), port));
    }

    Result<std::unique_ptr<ITransportChannel>> connect(
        const std::string& address, std::uint16_t port, const TransportCredentials&) override {
        auto fd = detail::make_tcp_connection(address, port);
        if (!fd) return Result<std::unique_ptr<ITransportChannel>>::failure(fd.error());
        return Result<std::unique_ptr<ITransportChannel>>::success(
            std::make_unique<ClearTcpChannel>(fd.value()));
    }
};

} // namespace

Result<std::unique_ptr<ITransportFactory>> make_clear_tcp_factory() {
    return Result<std::unique_ptr<ITransportFactory>>::success(
        std::make_unique<ClearTcpFactory>());
}

} // namespace plpt::net
