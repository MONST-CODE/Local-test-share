#pragma once
#include <cstddef>
#include <cstdint>
namespace plpt::model {
// Estimation expérimentale, PAS une télémétrie ni un résultat mesuré.
struct ResourceSample {
    double cpu_percent = 0, ram_used_mb = 0, ram_total_mb = 0, gpu_percent = 0;
    std::uint32_t active_profiles = 0;
};
enum class QualityLevel { Full720p, Reduced720p, Low480p, Paused };
struct CapacityDecision {
    bool accept_new_guest = false;
    QualityLevel quality = QualityLevel::Paused;
    std::size_t additional_guests = 0;
};
CapacityDecision decide_capacity(const ResourceSample&, std::size_t host_limit);
} // namespace plpt::model
