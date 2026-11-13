#include "registry.hpp"
#include <algorithm>
#include <set>
#include <tuple>
namespace plpt {
namespace {
std::string key(const SocketRecord& s, const ProcessRecord* p) {
    if (p && p->start_time_ticks != 0)
        return "p:" + std::to_string(p->pid.value) + ":" + std::to_string(p->start_time_ticks);
    return "e:" + std::to_string(static_cast<int>(s.protocol)) + ":" +
        std::to_string(static_cast<int>(s.local.family)) + ":" + s.local.address + ":" +
        std::to_string(s.local.scope_id) + ":" + std::to_string(s.local.port.value) + ":" +
        (s.owner_pid ? std::to_string(s.owner_pid->value) : "?");
}
}
Result<void> ServiceRegistry::ingest(const Observation<SocketRecord>& sockets,
    const Observation<ProcessRecord>& processes, std::uint64_t now_ms) {
    const bool complete = sockets.complete && processes.complete;
    const auto tree = ProcessTree::build(processes.records);
    std::map<std::string, ServiceCandidate> grouped;
    for (const auto& socket : sockets.records) {
        if (!is_service_endpoint(socket) || socket.local.port.value == 0) continue;
        const auto* node = socket.owner_pid ? tree.find(*socket.owner_pid) : nullptr;
        auto& group = grouped[key(socket, node ? &node->record : nullptr)];
        if (std::find(group.endpoints.begin(), group.endpoints.end(), socket) == group.endpoints.end())
            group.endpoints.push_back(socket);
        if (node) { group.process = node->record; group.origin = classify_origin(tree, node->record.pid); }
        if (grouped.size() > 4096 || group.endpoints.size() > 4096) {
            for (auto& [id, service] : services_) {
                (void)id;
                service.share_state = ShareState::Unavailable;
                service.selected_endpoint.reset();
            }
            return Result<void>::failure({ErrorCode::CapacityExceeded, "inventaire trop grand", "registry"});
        }
    }
    // Tombstones conservées une observation seulement : pas d'historique illimité.
    std::erase_if(services_, [&](const auto& item) {
        if (item.second.share_state != ShareState::Gone) return false;
        identities_.erase(item.first);
        return true;
    });
    std::map<std::string, ServiceId> old;
    for (const auto& [id, identity] : identities_) old.emplace(identity, id);
    std::set<ServiceId> seen;
    for (auto& [identity, group] : grouped) {
        const auto previous = old.find(identity);
        const bool stable = group.process && group.process->start_time_ticks != 0;
        const bool retain = previous != old.end();
        const auto id = retain ? previous->second : ServiceId{next_id_++};
        group.service_id = id;
        group.first_seen_ms = now_ms;
        group.last_seen_ms = now_ms;
        if (retain) {
            const auto& former = services_.at(id);
            group.first_seen_ms = former.first_seen_ms;
            if (stable && complete && former.share_state != ShareState::Unavailable) {
                if (former.share_state == ShareState::Protected) group.share_state = ShareState::Protected;
                if (former.selected_endpoint && std::any_of(group.endpoints.begin(), group.endpoints.end(), [&](const auto& ep) {
                    return ep.protocol == TransportProtocol::Tcp && ep.local == *former.selected_endpoint;
                })) {
                    group.selected_endpoint = former.selected_endpoint;
                    group.share_state = ShareState::Selected;
                }
            }
        }
        if (!complete) { group.share_state = ShareState::Unavailable; group.selected_endpoint.reset(); }
        seen.insert(id);
        identities_[id] = identity;
        services_[id] = std::move(group);
    }
    for (auto& [id, service] : services_) {
        if (!seen.contains(id)) {
            service.share_state = complete ? ShareState::Gone : ShareState::Unavailable;
            service.selected_endpoint.reset();
        }
    }
    // Les snapshots partiels répétés ne doivent pas accumuler des entrées
    // indisponibles indéfiniment. Éviction des anciennes, jamais d'un service vu.
    for (auto it = services_.begin(); services_.size() > 4096 && it != services_.end();) {
        if (!seen.contains(it->first)) {
            identities_.erase(it->first);
            it = services_.erase(it);
        } else ++it;
    }
    return Result<void>::success();
}
const ServiceCandidate* ServiceRegistry::find(ServiceId id) const {
    const auto it = services_.find(id);
    return it == services_.end() ? nullptr : &it->second;
}
Result<void> ServiceRegistry::select(ServiceId id, const Endpoint& endpoint) {
    const auto it = services_.find(id);
    if (it == services_.end()) return Result<void>::failure({ErrorCode::NotFound, "service inconnu", "registry"});
    auto& s = it->second;
    if (s.share_state != ShareState::Detected && s.share_state != ShareState::Selected)
        return Result<void>::failure({ErrorCode::InvalidState, "service non sélectionnable", "registry"});
    const bool found = std::any_of(s.endpoints.begin(), s.endpoints.end(), [&](const auto& e) {
        return e.protocol == TransportProtocol::Tcp && e.state == SocketState::Listening && e.local == endpoint;
    });
    if (!found) return Result<void>::failure({ErrorCode::InvalidArgument, "listener TCP requis", "registry"});
    s.share_state = ShareState::Selected;
    s.selected_endpoint = endpoint;
    return Result<void>::success();
}
Result<void> ServiceRegistry::protect(ServiceId id) {
    const auto it = services_.find(id);
    if (it == services_.end()) return Result<void>::failure({ErrorCode::NotFound, "service inconnu", "registry"});
    if (it->second.share_state == ShareState::Gone || it->second.share_state == ShareState::Unavailable)
        return Result<void>::failure({ErrorCode::InvalidState, "service absent", "registry"});
    it->second.share_state = ShareState::Protected;
    it->second.selected_endpoint.reset();
    return Result<void>::success();
}
std::size_t ServiceRegistry::live_count() const {
    return static_cast<std::size_t>(std::count_if(services_.begin(), services_.end(), [](const auto& item) {
        return item.second.share_state != ShareState::Gone && item.second.share_state != ShareState::Unavailable;
    }));
}
} // namespace plpt
