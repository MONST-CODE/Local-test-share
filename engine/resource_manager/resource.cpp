#include "resource.hpp"
#include <algorithm>
#include <cmath>
namespace plpt::model {
CapacityDecision decide_capacity(const ResourceSample& s, std::size_t limit) {
    CapacityDecision result;
    if (!std::isfinite(s.cpu_percent) || !std::isfinite(s.gpu_percent) || !std::isfinite(s.ram_total_mb) ||
        !std::isfinite(s.ram_used_mb) || s.cpu_percent < 0 || s.cpu_percent > 100 || s.gpu_percent < 0 ||
        s.gpu_percent > 100 || s.ram_used_mb < 0 || s.ram_total_mb < s.ram_used_mb ||
        limit == 0 || limit > 64 || s.active_profiles >= limit) return result;
    // Hypothèses non calibrées : 350 MiB/profil, 8 points CPU/profil, réserve 1 GiB.
    // Borner AVANT conversion double -> size_t, y compris entrée malformée/énorme.
    const auto ram = static_cast<std::size_t>(std::clamp((s.ram_total_mb - s.ram_used_mb - 1024) / 350, 0.0, 64.0));
    const auto cpu = static_cast<std::size_t>(std::clamp((100 - s.cpu_percent) / 8, 0.0, 64.0));
    result.additional_guests = std::min({limit - s.active_profiles, ram, cpu});
    result.accept_new_guest = result.additional_guests > 0;
    if (result.accept_new_guest) result.quality = s.cpu_percent > 85 || s.gpu_percent > 85 ? QualityLevel::Low480p :
        (s.cpu_percent > 65 ? QualityLevel::Reduced720p : QualityLevel::Full720p);
    return result;
}
} // namespace plpt::model
