// PLPT — engine/core/format.cpp — mise en forme déterministe, sans locale.
#include "format.hpp"

#include <cstdio>
#include <ctime>

namespace plpt {
namespace {
bool utc_parts(std::uint64_t wall_ms, std::tm& out) {
    const auto seconds = static_cast<std::time_t>(wall_ms / 1000U);
#ifdef _WIN32
    return ::gmtime_s(&out, &seconds) == 0;
#else
    return ::gmtime_r(&seconds, &out) != nullptr;
#endif
}
} // namespace

std::string format_utc(std::uint64_t wall_ms) {
    std::tm parts{};
    if (!utc_parts(wall_ms, parts)) return "horodatage invalide";
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d UTC", parts.tm_year + 1900, parts.tm_mon + 1,
        parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return buffer;
}

std::string format_clock_utc(std::uint64_t wall_ms) {
    std::tm parts{};
    if (!utc_parts(wall_ms, parts)) return "--:--:--";
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", parts.tm_hour, parts.tm_min, parts.tm_sec);
    return buffer;
}

std::string format_bytes(std::uint64_t bytes) {
    // Unités binaires : l'indice suit le nombre de divisions, jamais l'inverse.
    const char* const units[] = {"o", "Kio", "MiB", "GiB", "TiB"};
    constexpr std::size_t unit_count = sizeof(units) / sizeof(units[0]);
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < unit_count) { value /= 1024.0; ++unit; }
    char buffer[48];
    if (unit == 0) std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, units[unit]);
    else std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    return buffer;
}

std::string format_duration_ms(std::uint64_t milliseconds) {
    char buffer[48];
    if (milliseconds < 1000) {
        std::snprintf(buffer, sizeof(buffer), "%llu ms", static_cast<unsigned long long>(milliseconds));
        return buffer;
    }
    const auto seconds = milliseconds / 1000U;
    if (seconds < 60) {
        std::snprintf(buffer, sizeof(buffer), "%llu,%llu s", static_cast<unsigned long long>(seconds),
            static_cast<unsigned long long>((milliseconds % 1000U) / 100U));
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%llu min %llu s", static_cast<unsigned long long>(seconds / 60U),
        static_cast<unsigned long long>(seconds % 60U));
    return buffer;
}

std::string format_elapsed_ms(std::uint64_t elapsed_ms) { return format_duration_ms(elapsed_ms); }

} // namespace plpt
