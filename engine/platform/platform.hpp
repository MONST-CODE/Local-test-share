#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../core/types.hpp"
#include "../core/result.hpp"

namespace plpt {
// « Aucun résultat » != « observation impossible ». Un snapshot partiel ne
// peut pas prouver qu'un service a disparu. Les warnings ne contiennent pas de secrets.
template <typename T> struct Observation {
    std::vector<T> records;
    bool complete = true;
    std::vector<Error> warnings;
};
class IProcessEnumerator {
public:
    virtual ~IProcessEnumerator() = default;
    virtual Result<Observation<ProcessRecord>> enumerate() = 0;
};
class ISocketTableProvider {
public:
    virtual ~ISocketTableProvider() = default;
    virtual Result<Observation<SocketRecord>> snapshot() = 0;
};
struct NetworkInterfaceInfo {
    std::string name;
    std::string address;
    IpFamily family = IpFamily::Unknown;
    std::uint32_t scope_id = 0;
    bool is_up = false;
    bool is_loopback = false;
};
class INetworkInterfaceProvider {
public:
    virtual ~INetworkInterfaceProvider() = default;
    virtual Result<std::vector<NetworkInterfaceInfo>> enumerate() = 0;
};

// Contrat de stockage FUTUR : l'adaptateur devra vérifier propriété, lease,
// absence de processus navigateur et liens/reparse points avant suppression.
// Aucun sweep récursif ni création de profil n'est implémenté dans ce lot.
struct CleanupReport {
    bool complete = false;
    std::size_t removed_profiles = 0;
    std::vector<Error> failures;
};
class ITemporaryStorage {
public:
    virtual ~ITemporaryStorage() = default;
    virtual Result<std::string> create_session_dir(SessionId, ParticipantId) = 0;
    virtual Result<CleanupReport> destroy_session_dir(SessionId, ParticipantId) = 0;
    virtual Result<CleanupReport> sweep_verified_orphans() = 0;
};
class UnavailableTemporaryStorage final : public ITemporaryStorage {
public:
    Result<std::string> create_session_dir(SessionId, ParticipantId) override {
        return Result<std::string>::failure({ErrorCode::Unsupported, "stockage non qualifié", "storage"});
    }
    Result<CleanupReport> destroy_session_dir(SessionId, ParticipantId) override {
        return Result<CleanupReport>::failure({ErrorCode::Unsupported, "nettoyage non qualifié", "storage"});
    }
    Result<CleanupReport> sweep_verified_orphans() override {
        return Result<CleanupReport>::failure({ErrorCode::Unsupported, "audit des leases requis", "storage"});
    }
};
} // namespace plpt
