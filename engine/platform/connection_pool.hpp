#pragma once
// PLPT — engine/platform/connection_pool.hpp — pool borné de connexions acceptées.
//
// Un seul modèle de capacité, explicite et cohérent : au plus `workers` tâches en
// cours (les connexions ACTIVES) et au plus `queue_capacity` tâches EN ATTENTE.
// La capacité totale est donc `workers + queue_capacity`, et toute soumission
// au-delà est refusée — jamais une file infinie, jamais un thread détaché.
//
// Les deux notions sont observables séparément (`active()`, `queued()`, pics
// `max_active`/`max_queued`) : un service saturé peut dire s'il refuse parce que
// ses ouvriers sont occupés ou parce que sa file d'attente est pleine.
//
// La place d'ouvrier est réservée DÈS la soumission acceptée, pas au réveil de
// l'ouvrier : la classification `Active`/`Queued` ne dépend donc pas de la vitesse
// à laquelle le noyau ordonnance les threads, et `active() + queued()` reste borné
// par la capacité totale à tout instant observable.
//
// Tous les ouvriers sont joints à l'arrêt : aucun thread orphelin ne survit au
// processus. Le traitement doit être borné dans le temps (délais de socket), sinon
// l'arrêt attend la fin du traitement en cours — c'est déclaré, pas masqué.
//
// En-tête seul (définitions inline) : le service de démonstration et les tests
// d'intégration exercent exactement le même code, pas une copie.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "../core/result.hpp"
#include "socket.hpp"

namespace plpt {

struct PoolCounters {
    std::uint64_t submitted = 0;  // tâches acceptées (actives ou mises en file).
    std::uint64_t completed = 0;  // tâches terminées par un ouvrier.
    std::uint64_t refused = 0;    // soumissions refusées : capacité totale atteinte.
    std::uint64_t max_active = 0; // pic observé de connexions actives.
    std::uint64_t max_queued = 0; // pic observé de tâches en attente.
};

enum class PoolSubmission : std::uint8_t {
    Active,  // une place d'ouvrier était libre : elle est réservée à cette connexion.
    Queued,  // toutes les places sont prises : la connexion attend, dans la borne.
    Refused, // capacité totale atteinte : à signaler au client (503 par exemple).
};

class ConnectionPool {
public:
    using Handler = std::function<void(Socket&, const IpAddress&)>;
    using StatePredicate = std::function<bool(std::size_t active, std::size_t queued, const PoolCounters&)>;

    ConnectionPool() = default;
    ~ConnectionPool() { stop(); }
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    Result<void> start(std::size_t workers, std::size_t queue_capacity, Handler handler) {
        if (running_.load())
            return Result<void>::failure({ErrorCode::InvalidState, "pool déjà démarré", "pool"});
        if (workers == 0 || workers > 256)
            return Result<void>::failure({ErrorCode::InvalidArgument, "nombre d'ouvriers hors 1..256", "pool"});
        if (queue_capacity == 0 || queue_capacity > 4096)
            return Result<void>::failure({ErrorCode::InvalidArgument, "capacité de file hors 1..4096", "pool"});
        if (!handler)
            return Result<void>::failure({ErrorCode::InvalidArgument, "traitement des connexions absent", "pool"});
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            handler_ = std::move(handler);
            workers_ = workers;
            queue_capacity_ = queue_capacity;
            counters_ = PoolCounters{};
            queue_.clear();
            active_ = 0;
            queued_ = 0;
        }
        accepting_.store(true);
        running_.store(true);
        threads_.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) threads_.emplace_back([this] { worker_loop(); });
        return Result<void>::success();
    }

    // Prend la propriété de la socket SEULEMENT si la tâche est acceptée : en cas de
    // `Refused` l'appelant reste propriétaire et peut répondre (503) puis fermer.
    PoolSubmission submit(Socket&& socket, const IpAddress& peer) {
        if (!socket.valid()) return PoolSubmission::Refused;
        const std::lock_guard<std::mutex> guard(mutex_);
        if (!running_.load() || !accepting_.load()) {
            ++counters_.refused;
            return PoolSubmission::Refused;
        }
        if (active_ + queued_ >= workers_ + queue_capacity_) {
            ++counters_.refused;
            state_changed_.notify_all();
            return PoolSubmission::Refused;
        }
        // Réservation immédiate de la place d'ouvrier quand il en reste une : sinon
        // deux soumissions rapprochées pourraient être toutes deux classées « en
        // attente » alors qu'aucun ouvrier n'a encore eu le temps de se réveiller.
        const bool reserved = active_ < workers_;
        queue_.push_back(Task{std::move(socket), peer, reserved});
        ++counters_.submitted;
        if (reserved) {
            ++active_;
            if (active_ > counters_.max_active) counters_.max_active = active_;
        } else {
            ++queued_;
            if (queued_ > counters_.max_queued) counters_.max_queued = queued_;
        }
        state_changed_.notify_all();
        condition_.notify_one();
        return reserved ? PoolSubmission::Active : PoolSubmission::Queued;
    }

    // Arrêt : plus aucune acceptation, file vidée (les sockets non traitées sont
    // fermées par RAII), puis chaque ouvrier est joint. Aucun detach().
    void stop() {
        accepting_.store(false);
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            queue_.clear(); // les sockets non traitées sont fermées par RAII.
            queued_ = 0;
            state_changed_.notify_all();
        }
        condition_.notify_all();
        for (auto& worker : threads_)
            if (worker.joinable()) worker.join();
        threads_.clear();
        running_.store(false);
    }

    std::size_t active() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return active_;
    }
    std::size_t queued() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return queued_;
    }
    std::size_t capacity() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return workers_ + queue_capacity_;
    }
    PoolCounters counters() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return counters_;
    }
    bool running() const { return running_.load(); }

    // Attente bornée sur un état réel : jamais de polling actif dans les tests.
    // Le prédicat reçoit l'état déjà verrouillé, il ne doit pas rappeler active()
    // ou queued() (cela se verrouillerait deux fois).
    bool wait_for(const StatePredicate& predicate, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> guard(mutex_);
        return state_changed_.wait_for(guard, timeout, [&] { return predicate(active_, queued_, counters_); });
    }

private:
    struct Task {
        Socket socket;
        IpAddress peer;
        bool reserved = false; // place d'ouvrier déjà comptée dans active_.
    };

    void worker_loop() {
        for (;;) {
            Task task{};
            {
                std::unique_lock<std::mutex> guard(mutex_);
                condition_.wait(guard, [this] { return !queue_.empty() || !accepting_.load(); });
                if (queue_.empty()) return; // arrêt demandé : plus rien à traiter.
                task = std::move(queue_.front());
                queue_.pop_front();
                // Une tâche réservée est déjà comptée dans active_ ; une tâche en
                // attente occupe maintenant cet ouvrier, devenu libre.
                if (!task.reserved) {
                    --queued_;
                    ++active_;
                    if (active_ > counters_.max_active) counters_.max_active = active_;
                }
                state_changed_.notify_all();
            }
            if (handler_) handler_(task.socket, task.peer);
            task.socket.close(); // la fermeture est aussi assurée par RAII.
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                --active_;
                ++counters_.completed;
                state_changed_.notify_all();
            }
            condition_.notify_all(); // un ouvrier libéré peut prendre la suite.
        }
    }

    Handler handler_;
    std::size_t workers_ = 0;
    std::size_t queue_capacity_ = 0;
    std::size_t active_ = 0;
    std::size_t queued_ = 0;   // tâches en attente d'une place d'ouvrier libre.
    std::deque<Task> queue_;
    PoolCounters counters_{};
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<bool> accepting_{false};

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;       // réveil des ouvriers.
    mutable std::condition_variable state_changed_;   // réveil des observateurs.
};

} // namespace plpt
