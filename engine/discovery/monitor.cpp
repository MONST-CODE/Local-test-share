#include "monitor.hpp"
#include <algorithm>
#include <stdexcept>
namespace plpt {
Result<Inventory> observe(IProcessEnumerator& processes, ISocketTableProvider& sockets) {
    auto before = processes.enumerate();
    if (!before) return Result<Inventory>::failure(before.error());
    auto table = sockets.snapshot();
    if (!table) return Result<Inventory>::failure(table.error());
    auto after = processes.enumerate();
    if (!after) return Result<Inventory>::failure(after.error());
    std::map<ProcessId, std::uint64_t> identities;
    for (const auto& p : before.value().records) identities[p.pid] = p.start_time_ticks;
    std::map<ProcessId, std::uint64_t> stable;
    for (const auto& p : after.value().records) {
        const auto it = identities.find(p.pid);
        if (it != identities.end() && it->second != 0 && it->second == p.start_time_ticks) stable[p.pid] = p.start_time_ticks;
    }
    for (auto& socket : table.value().records)
        if (socket.owner_pid && !stable.contains(*socket.owner_pid)) socket.owner_pid.reset();
    after.value().complete = after.value().complete && before.value().complete;
    after.value().warnings.insert(after.value().warnings.end(), before.value().warnings.begin(), before.value().warnings.end());
    return Result<Inventory>::success({std::move(table).value(), std::move(after).value()});
}
Monitor::Monitor(std::unique_ptr<IProcessEnumerator> processes, std::unique_ptr<ISocketTableProvider> sockets)
    : processes_(std::move(processes)), sockets_(std::move(sockets)), view_(std::make_shared<MonitorView>()) {
    if (!processes_ || !sockets_) throw std::invalid_argument("monitor provider missing");
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}
Monitor::~Monitor() {
    worker_.request_stop();
    ready_.notify_all();
    worker_.join();
}
std::shared_ptr<const MonitorView> Monitor::view() const {
    std::lock_guard lock(mutex_);
    return view_;
}
bool Monitor::enqueue(Command command) {
    {
        std::lock_guard lock(mutex_);
        if (commands_.size() >= 32) return false;
        commands_.push_back(std::move(command));
    }
    ready_.notify_all();
    return true;
}
bool Monitor::request_refresh() { return enqueue({Kind::Refresh, {}, {}}); }
bool Monitor::request_selection(ServiceId id, Endpoint e) { return enqueue({Kind::Select, id, std::move(e)}); }
bool Monitor::request_protection(ServiceId id) { return enqueue({Kind::Protect, id, {}}); }
bool Monitor::wait_for_revision(std::uint64_t min, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, timeout, [&] { return view_->revision >= min; });
}
void Monitor::run(std::stop_token stop) {
    ServiceRegistry registry;
    SystemClock clock;
    std::uint64_t revision = 0;
    while (!stop.stop_requested()) {
        std::deque<Command> pending;
        { std::lock_guard lock(mutex_); pending.swap(commands_); }
        auto next = std::make_shared<MonitorView>();
        next->revision = ++revision;
        auto inventory = observe(*processes_, *sockets_);
        if (inventory) {
            auto& data = inventory.value();
            next->complete = data.sockets.complete && data.processes.complete;
            next->warnings = data.sockets.warnings;
            next->warnings.insert(next->warnings.end(), data.processes.warnings.begin(), data.processes.warnings.end());
            if (auto r = registry.ingest(data.sockets, data.processes, clock.now_monotonic_ms()); !r) {
                next->complete = false;
                next->warnings.push_back(r.error());
            }
        } else {
            next->warnings.push_back(inventory.error());
            // Échec d'observation : ne jamais conserver une sélection « saine ».
            Observation<SocketRecord> unavailable;
            unavailable.complete = false;
            const auto r = registry.ingest(unavailable, {}, clock.now_monotonic_ms());
            if (!r) next->warnings.push_back(r.error());
        }
        for (const auto& command : pending) {
            if (command.kind == Kind::Refresh) continue;
            auto r = command.kind == Kind::Select ? registry.select(command.id, command.endpoint) : registry.protect(command.id);
            if (!r) next->warnings.push_back(r.error());
        }
        for (const auto& [id, service] : registry.services()) { (void)id; next->services.push_back(service); }
        {
            std::lock_guard lock(mutex_);
            view_ = std::move(next); // une seule vue retenue, aucune file de snapshots infinie.
        }
        ready_.notify_all();
        std::unique_lock lock(mutex_);
        ready_.wait_for(lock, std::chrono::seconds(2), [&] { return stop.stop_requested() || !commands_.empty(); });
    }
}
} // namespace plpt
