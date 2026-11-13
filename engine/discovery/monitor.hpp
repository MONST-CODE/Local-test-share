#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "../core/clock.hpp"
#include "../service_registry/registry.hpp"
namespace plpt {
struct Inventory {
    Observation<SocketRecord> sockets;
    Observation<ProcessRecord> processes;
};
// Bracketing : associer un PID seulement si la même création est observée
// avant ET après la table sockets. Ce n'est pas une transaction OS atomique.
Result<Inventory> observe(IProcessEnumerator&, ISocketTableProvider&);
struct MonitorView {
    std::uint64_t revision = 0;
    std::vector<ServiceCandidate> services;
    std::vector<Error> warnings;
    bool complete = false;
};
class Monitor {
public:
    Monitor(std::unique_ptr<IProcessEnumerator>, std::unique_ptr<ISocketTableProvider>);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    std::shared_ptr<const MonitorView> view() const;
    bool request_refresh();
    bool request_selection(ServiceId, Endpoint);
    bool request_protection(ServiceId);
    // Utilisé par CLI/tests : attente bornée, pas de polling actif.
    bool wait_for_revision(std::uint64_t minimum, std::chrono::milliseconds timeout);
private:
    enum class Kind { Refresh, Select, Protect };
    struct Command { Kind kind; ServiceId id{}; Endpoint endpoint{}; };
    bool enqueue(Command);
    void run(std::stop_token);
    std::unique_ptr<IProcessEnumerator> processes_;
    std::unique_ptr<ISocketTableProvider> sockets_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Command> commands_;
    std::shared_ptr<const MonitorView> view_;
    std::jthread worker_; // détruit avant les primitives de synchronisation.
};
} // namespace plpt
