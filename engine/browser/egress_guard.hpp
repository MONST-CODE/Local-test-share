#pragma once
// DevLink — engine/browser/egress_guard.hpp — socket-level network boundary of
// the Browser Worker (ADR-018 §4).
//
// Request interception (WebResourceRequested) is NOT a network boundary: the
// Chromium network stack opens speculative connections (preconnect) before any
// request exists, and WebSockets are not intercepted at all — the hostile test
// proved real TCP connections reaching a forbidden port. The browser is
// therefore started with `--proxy-server=socks5://127.0.0.1:<guard>` and
// `--proxy-bypass-list=<-loopback>`: EVERY connection it makes (HTTP, HTTPS,
// WebSocket, preconnect, loopback included) is a SOCKS5 CONNECT that this guard
// accepts only for an allowlisted host:port, after which bytes are spliced.
//
//  - Hosts: IPv4/IPv6 literals and `localhost` (mapped to 127.0.0.1 then ::1).
//    Other DNS names are refused explicitly: the browser never resolves names
//    itself (SOCKS5 remote resolution) and the guard does not either, which
//    removes DNS rebinding by construction.
//  - Listens on 127.0.0.1 only, ephemeral port, bounded concurrent tunnels.
//  - Counters for diagnostics; denied targets are never logged with secrets
//    (only host:port, which the host itself typed or the page requested).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../core/result.hpp"

namespace plpt::browser {

struct EgressTarget {
    std::string host; // canonical: "127.0.0.1", "::1", "localhost"
    std::uint16_t port = 0;
    auto operator<=>(const EgressTarget&) const = default;
};

// "http://127.0.0.1:3000" -> {127.0.0.1, 3000}; nullopt for anything that is
// not an exact canonical http(s) origin.
std::optional<EgressTarget> egress_target_of_origin(const std::string& origin);

struct EgressCounters {
    std::uint64_t allowed = 0;
    std::uint64_t denied = 0;
    std::uint64_t failed_upstream = 0;
    std::uint64_t bytes_up = 0;
    std::uint64_t bytes_down = 0;
    std::size_t active = 0;
    std::vector<std::string> recent_denied; // "host:port", bounded
};

class EgressGuard {
public:
    EgressGuard();
    ~EgressGuard();
    EgressGuard(const EgressGuard&) = delete;
    EgressGuard& operator=(const EgressGuard&) = delete;

    Result<std::uint16_t> start(std::size_t max_tunnels = 64);
    void stop(); // idempotent, joins every thread
    std::uint16_t port() const;

    // Reference-counted allowlist: add when a context is created with a policy,
    // remove when it is destroyed. Only canonical origins are accepted.
    Result<void> add_origins(const std::vector<std::string>& origins);
    void remove_origins(const std::vector<std::string>& origins);
    bool allowed(const EgressTarget& target) const;
    EgressCounters counters() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace plpt::browser
