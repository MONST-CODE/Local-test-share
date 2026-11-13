#pragma once
#include <map>
#include <optional>
#include <vector>
#include "../platform/platform.hpp"
#include "../discovery/terminal_classifier.hpp"

namespace plpt {
struct ServiceCandidate {
    ServiceId service_id{};
    std::vector<SocketRecord> endpoints;
    std::optional<ProcessRecord> process;
    OriginInference origin;
    ShareState share_state = ShareState::Detected;
    std::optional<Endpoint> selected_endpoint;
    std::uint64_t first_seen_ms = 0, last_seen_ms = 0;
};
// Registre d'OBSERVATION, pas moteur d'autorisation. Plusieurs endpoints par
// identité de processus. Les projets multi-processus attendent service_graph.
class ServiceRegistry {
public:
    // Les deux complétudes sont nécessaires : ne jamais réduire l'observation
    // processus à ses seuls records en oubliant une lecture interrompue.
    Result<void> ingest(const Observation<SocketRecord>&, const Observation<ProcessRecord>&, std::uint64_t now_ms);
    const std::map<ServiceId, ServiceCandidate>& services() const { return services_; }
    const ServiceCandidate* find(ServiceId) const;
    Result<void> select(ServiceId, const Endpoint&); // sélection locale sans effet réseau.
    Result<void> protect(ServiceId);
    std::size_t live_count() const;
private:
    std::map<ServiceId, ServiceCandidate> services_;
    std::map<ServiceId, std::string> identities_;
    std::uint64_t next_id_ = 1;
};
} // namespace plpt
