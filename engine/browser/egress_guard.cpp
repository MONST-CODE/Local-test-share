// DevLink — engine/browser/egress_guard.cpp — see egress_guard.hpp.
// SOCKS5 subset implemented: RFC 1928 method 0x00 (no auth, loopback only),
// CMD CONNECT, ATYP IPv4/DOMAIN/IPv6. Everything else is refused.
#include "egress_guard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <list>
#include <map>
#include <optional>
#include <thread>

#include "../platform/socket.hpp"
#include "url_origin.hpp"

namespace plpt::browser {

using namespace std::chrono_literals;

std::optional<EgressTarget> egress_target_of_origin(const std::string& origin) {
    const auto canonical = url_origin::canonicalize(origin, true);
    if (!canonical) return std::nullopt;
    const std::string& c = *canonical;
    const bool https = c.rfind("https://", 0) == 0;
    const std::size_t start = https ? 8 : 7;
    std::string authority = c.substr(start);
    if (!authority.empty() && authority.back() == '/') authority.pop_back();
    EgressTarget target;
    std::string port_text;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return std::nullopt;
        target.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return std::nullopt;
            port_text = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        target.host = colon == std::string::npos ? authority : authority.substr(0, colon);
        if (colon != std::string::npos) port_text = authority.substr(colon + 1);
    }
    unsigned long port = https ? 443 : 80;
    if (!port_text.empty()) {
        char* end = nullptr;
        port = std::strtoul(port_text.c_str(), &end, 10);
        if (*end != '\0' || port == 0 || port > 65535) return std::nullopt;
    }
    target.port = static_cast<std::uint16_t>(port);
    if (target.host.empty()) return std::nullopt;
    return target;
}

namespace {
constexpr std::size_t kMaxRecentDenied = 32;
constexpr auto kHandshakeTimeout = 5000ms;

std::string ipv6_text(const std::uint8_t* b) {
    // Canonical RFC 5952 text through the url_origin canonicaliser.
    char raw[64];
    std::snprintf(raw, sizeof raw, "%x:%x:%x:%x:%x:%x:%x:%x", (b[0] << 8) | b[1], (b[2] << 8) | b[3],
        (b[4] << 8) | b[5], (b[6] << 8) | b[7], (b[8] << 8) | b[9], (b[10] << 8) | b[11], (b[12] << 8) | b[13],
        (b[14] << 8) | b[15]);
    const auto canonical = url_origin::canonicalize(std::string("http://[") + raw + "]", true);
    if (!canonical) return raw;
    const auto open = canonical->find('[');
    const auto close = canonical->find(']');
    return canonical->substr(open + 1, close - open - 1);
}

bool wait_io(Socket& socket, bool write, std::chrono::milliseconds timeout) {
    PollEntry entry;
    entry.socket = socket.native();
    entry.want_read = !write;
    entry.want_write = write;
    auto r = poll_sockets(std::span<PollEntry>(&entry, 1), timeout);
    return r && r.value() > 0 && !entry.failed && (write ? entry.writable : entry.readable);
}

bool read_exact(Socket& socket, std::uint8_t* out, std::size_t n, std::chrono::steady_clock::time_point deadline) {
    std::size_t got = 0;
    while (got < n) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0 || !wait_io(socket, false, left)) return false;
        auto r = socket.receive(std::span<std::uint8_t>(out + got, n - got));
        if (!r) {
            if (r.error().code == ErrorCode::WouldBlock) continue;
            return false;
        }
        if (r.value() == 0) return false;
        got += r.value();
    }
    return true;
}

bool write_all(Socket& socket, const std::uint8_t* data, std::size_t n, const std::atomic<bool>& stop) {
    std::size_t sent = 0;
    while (sent < n) {
        if (stop.load()) return false;
        auto r = socket.send(std::span<const std::uint8_t>(data + sent, n - sent));
        if (!r) {
            if (r.error().code != ErrorCode::WouldBlock) return false;
            if (!wait_io(socket, true, 1000ms)) {
                if (stop.load()) return false;
            }
            continue;
        }
        sent += r.value();
    }
    return true;
}
} // namespace

struct EgressGuard::Impl {
    Socket listener;
    std::uint16_t port = 0;
    std::size_t max_tunnels = 64;
    std::atomic<bool> stop{false};
    std::thread acceptor;
    mutable std::mutex mutex;
    std::map<EgressTarget, unsigned> allow; // reference counted
    EgressCounters counters;
    struct Tunnel {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };
    std::list<Tunnel> tunnels;

    void deny(const EgressTarget& t) {
        std::lock_guard lock(mutex);
        counters.denied++;
        if (counters.recent_denied.size() >= kMaxRecentDenied) counters.recent_denied.erase(counters.recent_denied.begin());
        counters.recent_denied.push_back(t.host + ":" + std::to_string(t.port));
    }
    bool is_allowed(const EgressTarget& t) const {
        std::lock_guard lock(mutex);
        auto it = allow.find(t);
        return it != allow.end() && it->second > 0;
    }

    void accept_loop() {
        while (!stop.load()) {
            IpAddress peer;
            Port peer_port{};
            auto accepted = listener.accept(&peer, &peer_port);
            if (!accepted) {
                if (!wait_io(listener, false, 100ms)) continue;
                continue;
            }
            Socket client = std::move(accepted).value();
            prune();
            std::lock_guard lock(mutex);
            if (counters.active >= max_tunnels || !peer.is_loopback()) {
                counters.denied++;
                client.close();
                continue;
            }
            counters.active++;
            tunnels.emplace_back();
            auto& tunnel = tunnels.back();
            auto done = tunnel.done;
            tunnel.thread = std::thread([this, done, s = std::move(client)]() mutable {
                serve(std::move(s));
                {
                    std::lock_guard l(mutex);
                    counters.active--;
                }
                done->store(true);
            });
        }
    }

    void prune() {
        std::list<Tunnel> finished;
        {
            std::lock_guard lock(mutex);
            for (auto it = tunnels.begin(); it != tunnels.end();) {
                if (it->done->load()) {
                    auto next = std::next(it);
                    finished.splice(finished.end(), tunnels, it);
                    it = next;
                } else {
                    ++it;
                }
            }
        }
        for (auto& t : finished)
            if (t.thread.joinable()) t.thread.join();
    }

    void serve(Socket client) {
        (void)client.set_nonblocking(true);
        const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        std::uint8_t head[2];
        if (!read_exact(client, head, 2, deadline) || head[0] != 5 || head[1] == 0) return;
        std::array<std::uint8_t, 255> methods{};
        if (!read_exact(client, methods.data(), head[1], deadline)) return;
        const bool no_auth = std::find(methods.begin(), methods.begin() + head[1], 0) != methods.begin() + head[1];
        const std::uint8_t choice[2] = {5, static_cast<std::uint8_t>(no_auth ? 0x00 : 0xFF)};
        if (!write_all(client, choice, 2, stop) || !no_auth) return;
        std::uint8_t req[4];
        if (!read_exact(client, req, 4, deadline) || req[0] != 5 || req[2] != 0) return;
        auto reply = [&](std::uint8_t code) {
            const std::uint8_t r[10] = {5, code, 0, 1, 0, 0, 0, 0, 0, 0};
            (void)write_all(client, r, sizeof r, stop);
        };
        EgressTarget target;
        std::uint8_t addr[256];
        if (req[3] == 1) {
            if (!read_exact(client, addr, 4, deadline)) return;
            char text[16];
            std::snprintf(text, sizeof text, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
            target.host = text;
        } else if (req[3] == 4) {
            if (!read_exact(client, addr, 16, deadline)) return;
            target.host = ipv6_text(addr);
        } else if (req[3] == 3) {
            std::uint8_t len = 0;
            if (!read_exact(client, &len, 1, deadline) || len == 0 || !read_exact(client, addr, len, deadline)) return;
            target.host.assign(reinterpret_cast<const char*>(addr), len);
            std::transform(target.host.begin(), target.host.end(), target.host.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // A bracketed or numeric name is re-canonicalised so that e.g.
            // "127.000.0.1" can never alias an allowlisted literal.
            if (auto canon = egress_target_of_origin("http://" + target.host + ":1")) target.host = canon->host;
            else target.host = "\x01invalid";
        } else {
            reply(0x08); // address type not supported
            return;
        }
        std::uint8_t port_bytes[2];
        if (!read_exact(client, port_bytes, 2, deadline)) return;
        target.port = static_cast<std::uint16_t>((port_bytes[0] << 8) | port_bytes[1]);
        if (req[1] != 1 || !is_allowed(target)) {
            deny(target);
            reply(req[1] != 1 ? 0x07 : 0x02); // command not supported / not allowed by ruleset
            return;
        }
        // Resolution without DNS: literals as-is, localhost -> loopback v4 then v6.
        std::vector<IpAddress> candidates;
        if (target.host == "localhost") {
            candidates.push_back(IpAddress::loopback(IpFamily::Ipv4));
            candidates.push_back(IpAddress::loopback(IpFamily::Ipv6));
        } else if (auto parsed = IpAddress::parse(target.host)) {
            candidates.push_back(parsed.value());
        } else {
            deny(target); // allowlisted DNS name: refused, never resolved (see header)
            reply(0x02);
            return;
        }
        Socket upstream;
        bool connected = false;
        for (const auto& address : candidates) {
            auto s = Socket::create(TransportProtocol::Tcp, address.family);
            if (!s) continue;
            upstream = std::move(s).value();
            if (connect_with_timeout(upstream, address, Port{target.port}, 3000ms)) { connected = true; break; }
            upstream.close();
        }
        if (!connected) {
            std::lock_guard lock(mutex);
            counters.failed_upstream++;
            reply(0x05); // connection refused
            return;
        }
        (void)upstream.set_nonblocking(true);
        (void)upstream.set_no_delay(true);
        (void)client.set_no_delay(true);
        {
            std::lock_guard lock(mutex);
            counters.allowed++;
        }
        reply(0x00);
        splice(client, upstream);
    }

    void splice(Socket& client, Socket& upstream) {
        std::array<std::uint8_t, 16384> buffer{};
        std::uint64_t up = 0, down = 0;
        for (;;) {
            if (stop.load()) break;
            PollEntry entries[2];
            entries[0].socket = client.native();
            entries[0].want_read = true;
            entries[1].socket = upstream.native();
            entries[1].want_read = true;
            auto r = poll_sockets(std::span<PollEntry>(entries, 2), 250ms);
            if (!r) break;
            if (entries[0].failed || entries[1].failed) break;
            bool closed = false;
            for (int i = 0; i < 2 && !closed; ++i) {
                if (!entries[i].readable) continue;
                Socket& from = i == 0 ? client : upstream;
                Socket& to = i == 0 ? upstream : client;
                auto got = from.receive(buffer);
                if (!got) {
                    if (got.error().code == ErrorCode::WouldBlock) continue;
                    closed = true;
                    break;
                }
                if (got.value() == 0 || !write_all(to, buffer.data(), got.value(), stop)) { closed = true; break; }
                (i == 0 ? up : down) += got.value();
            }
            if (closed) break;
        }
        std::lock_guard lock(mutex);
        counters.bytes_up += up;
        counters.bytes_down += down;
    }
};

EgressGuard::EgressGuard() : impl_(std::make_unique<Impl>()) {}
EgressGuard::~EgressGuard() { stop(); }

Result<std::uint16_t> EgressGuard::start(std::size_t max_tunnels) {
    if (impl_->port != 0) return Result<std::uint16_t>::success(impl_->port);
    impl_->max_tunnels = std::max<std::size_t>(1, max_tunnels);
    auto s = Socket::create(TransportProtocol::Tcp, IpFamily::Ipv4);
    if (!s) return Result<std::uint16_t>::failure(s.error());
    impl_->listener = std::move(s).value();
    (void)impl_->listener.set_exclusive_bind(true);
    if (auto b = impl_->listener.bind(IpAddress::loopback(IpFamily::Ipv4), Port{0}); !b)
        return Result<std::uint16_t>::failure(b.error());
    if (auto l = impl_->listener.listen(64); !l) return Result<std::uint16_t>::failure(l.error());
    if (auto n = impl_->listener.set_nonblocking(true); !n) return Result<std::uint16_t>::failure(n.error());
    auto p = impl_->listener.bound_port();
    if (!p) return Result<std::uint16_t>::failure(p.error());
    impl_->port = p.value().value;
    impl_->stop.store(false);
    impl_->acceptor = std::thread([this] { impl_->accept_loop(); });
    return Result<std::uint16_t>::success(impl_->port);
}

void EgressGuard::stop() {
    if (!impl_) return;
    impl_->stop.store(true);
    if (impl_->acceptor.joinable()) impl_->acceptor.join();
    std::list<Impl::Tunnel> all;
    {
        std::lock_guard lock(impl_->mutex);
        all.swap(impl_->tunnels);
    }
    for (auto& t : all)
        if (t.thread.joinable()) t.thread.join();
    impl_->listener.close();
    impl_->port = 0;
}

std::uint16_t EgressGuard::port() const { return impl_->port; }

Result<void> EgressGuard::add_origins(const std::vector<std::string>& origins) {
    std::vector<EgressTarget> targets;
    for (const auto& o : origins) {
        auto t = egress_target_of_origin(o);
        if (!t) return Result<void>::failure(Error{ErrorCode::InvalidArgument, "origine non canonique refusée", "egress"});
        targets.push_back(*t);
    }
    std::lock_guard lock(impl_->mutex);
    for (const auto& t : targets) impl_->allow[t]++;
    return Result<void>::success();
}

void EgressGuard::remove_origins(const std::vector<std::string>& origins) {
    std::lock_guard lock(impl_->mutex);
    for (const auto& o : origins) {
        auto t = egress_target_of_origin(o);
        if (!t) continue;
        auto it = impl_->allow.find(*t);
        if (it != impl_->allow.end() && --it->second == 0) impl_->allow.erase(it);
    }
}

bool EgressGuard::allowed(const EgressTarget& target) const { return impl_->is_allowed(target); }

EgressCounters EgressGuard::counters() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

} // namespace plpt::browser
