#pragma once
// PLPT — engine/core/clock.hpp — IClock (§8.1) : horloge injectable, testable.
#include <chrono>
#include <cstdint>

namespace plpt {

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::uint64_t now_monotonic_ms() const = 0;
    virtual std::uint64_t now_wall_ms() const = 0;
};

class SystemClock final : public IClock {
public:
    std::uint64_t now_monotonic_ms() const override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    std::uint64_t now_wall_ms() const override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
};

// Horloge manuelle pour tests déterministes (expiration, heartbeat, timeouts).
class ManualClock final : public IClock {
public:
    explicit ManualClock(std::uint64_t start_ms = 0) : now_(start_ms) {}
    std::uint64_t now_monotonic_ms() const override { return now_; }
    std::uint64_t now_wall_ms() const override { return now_; }
    void advance_ms(std::uint64_t delta) { now_ += delta; }
    void set_ms(std::uint64_t v) { now_ = v; }
private:
    std::uint64_t now_;
};

} // namespace plpt
