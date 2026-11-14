#pragma once
// PLPT — engine/net/discovery.hpp — annonce et écoute des sessions sur le LAN.
// UDP multicast (239.255.77.88, scope administrable, jamais routé) + repli
// broadcast + cible loopback explicite pour la démonstration sur une machine.
// Une annonce n'est JAMAIS une preuve d'identité ni une autorisation : rejoindre
// exige le code d'invitation affiché par l'hôte.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../core/clock.hpp"
#include "../core/result.hpp"
#include "../platform/socket.hpp"

namespace plpt::net {

inline constexpr char kDiscoveryGroup[] = "239.255.77.88";
inline constexpr std::uint16_t kDiscoveryPort = 47788;
inline constexpr std::uint16_t kAnnounceVersion = 1;
inline constexpr std::size_t kMaxAnnouncementBytes = 512;
inline constexpr std::uint32_t kAnnounceIntervalMs = 2000;
inline constexpr std::uint32_t kAnnounceExpiryMs = 3 * kAnnounceIntervalMs + 1500;

struct Announcement {
    std::uint64_t session_id = 0;
    std::string session_name;
    std::string host_name;
    std::uint16_t control_port = 0;
    std::uint16_t participants = 0;
    std::uint16_t max_participants = 0;
    std::uint8_t shared_services = 0;
    bool accepting = false;
    std::uint32_t sequence = 0;
    std::uint64_t wall_ms = 0;
};

Result<std::vector<std::uint8_t>> encode_announcement(const Announcement&);
Result<Announcement> decode_announcement(std::span<const std::uint8_t> datagram);

struct DiscoveredSession {
    std::uint64_t session_id = 0;
    std::string session_name;
    std::string host_name;
    IpAddress address;      // source réelle du datagramme, jamais un champ annoncé.
    std::uint16_t control_port = 0;
    std::size_t participants = 0;
    std::size_t max_participants = 0;
    std::size_t shared_services = 0;
    bool accepting = false;
    std::uint32_t sequence = 0;
    std::uint64_t first_seen_ms = 0;
    std::uint64_t last_seen_ms = 0;
    bool manual = false; // saisie manuelle : n'expire pas, n'est jamais une preuve.
    // Une saisie manuelle n'expire jamais : aucun datagramme ne peut la rafraîchir,
    // et la supprimer ferait disparaître la seule adresse que l'utilisateur connaît.
    bool expired(std::uint64_t now_ms) const {
        if (manual) return false;
        return now_ms >= last_seen_ms && now_ms - last_seen_ms > kAnnounceExpiryMs;
    }
};

class DiscoveryListener {
public:
    struct Config {
        IpAddress bind = IpAddress::any(IpFamily::Ipv4);
        Port port{0};                      // 0 = éphémère (tests), kDiscoveryPort en usage normal.
        bool join_multicast = true;
        bool allow_broadcast = true;
    };
    explicit DiscoveryListener(IClock& clock) : clock_(clock) {}
    ~DiscoveryListener();
    DiscoveryListener(const DiscoveryListener&) = delete;
    DiscoveryListener& operator=(const DiscoveryListener&) = delete;

    Result<void> start(const Config& config);
    void stop();
    bool running() const { return running_.load(); }
    Port bound_port() const { return bound_port_; }
    std::shared_ptr<const std::vector<DiscoveredSession>> snapshot() const;
    std::vector<Error> warnings() const;
    std::uint64_t accepted_datagrams() const { return accepted_.load(); }
    std::uint64_t rejected_datagrams() const { return rejected_.load(); }
    // Saisie manuelle d'une adresse : même statut qu'une annonce, aucune confiance ajoutée.
    void inject_manual(IpAddress address, Port control_port, std::string session_name);
    // Attente bornée utilisée par les tests et l'interface : pas de polling actif.
    bool wait_for_sessions(std::size_t minimum, std::chrono::milliseconds timeout);

private:
    void run(std::stop_token token);
    void publish();
    IClock& clock_;
    Config config_;
    Socket socket_;
    WakeupSignal wakeup_;
    Port bound_port_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<DiscoveredSession> sessions_;
    std::vector<Error> warnings_;
    // Toute mutation incrémente revision_ ; la vue immuable n'est reconstruite
    // que si elle diffère, sinon chaque datagramme reallocerait le snapshot.
    std::uint64_t revision_ = 0;
    std::uint64_t published_revision_ = 0;
    std::shared_ptr<const std::vector<DiscoveredSession>> view_;
    std::jthread worker_;
};

class Announcer {
public:
    struct Config {
        Announcement announcement;
        IpAddress source = IpAddress::any(IpFamily::Ipv4);
        std::vector<IpAddress> interfaces;             // interfaces LAN pour le multicast.
        std::vector<std::pair<IpAddress, Port>> extra_targets; // loopback, broadcast, tests.
        int ttl = 4;
        bool multicast_loop = true;
        std::uint32_t interval_ms = kAnnounceIntervalMs;
    };
    explicit Announcer(IClock& clock) : clock_(clock) {}
    ~Announcer();
    Announcer(const Announcer&) = delete;
    Announcer& operator=(const Announcer&) = delete;

    Result<void> start(Config config);
    void stop();
    bool running() const { return running_.load(); }
    void update(Announcement announcement);
    // Émission immédiate et déterministe : utilisée par les tests et au démarrage.
    Result<std::size_t> send_now();
    std::uint64_t sent_datagrams() const { return sent_.load(); }
    std::vector<Error> failures() const;

private:
    void run(std::stop_token token);
    Result<void> prepare();
    IClock& clock_;
    Config config_;
    Socket socket_;
    WakeupSignal wakeup_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> sent_{0};
    mutable std::mutex mutex_;
    std::vector<Error> failures_;
    std::vector<std::pair<IpAddress, Port>> targets_;
    std::uint32_t sequence_ = 0;
    std::jthread worker_;
};

// Cibles réelles d'annonce : multicast par interface LAN + loopback (démo sur une
// seule machine) + broadcast si le multicast n'est pas disponible.
Announcer::Config announce_targets(const std::vector<NetworkInterfaceInfo>& interfaces, const Announcement& announcement,
    Port discovery_port, bool include_loopback);

} // namespace plpt::net
