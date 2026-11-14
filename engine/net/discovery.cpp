// PLPT — engine/net/discovery.cpp — annonce et écoute LAN.
#include "discovery.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include "../core/text.hpp"
#include "wire.hpp"

namespace plpt::net {
namespace {
constexpr std::array<char, 8> kMagic{'P', 'L', 'P', 'T', 'D', 'S', 'C', '1'};
constexpr std::size_t kMaxNameInAnnouncement = 48;

Error malformed() { return {ErrorCode::ProtocolViolation, "annonce de découverte invalide", "discovery"}; }

void put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned bytes) {
    for (unsigned index = bytes; index > 0; --index) out.push_back(static_cast<std::uint8_t>(value >> ((index - 1) * 8)));
}
std::uint64_t take(std::span<const std::uint8_t> data, std::size_t& offset, unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < bytes; ++index) value = (value << 8) | data[offset++];
    return value;
}

bool same_session(const DiscoveredSession& left, const DiscoveredSession& right) {
    if (left.session_id != right.session_id) return false;
    if (left.control_port != right.control_port) return false;
    return left.address.text == right.address.text && left.address.family == right.address.family &&
        left.address.scope_id == right.address.scope_id;
}
} // namespace

Result<std::vector<std::uint8_t>> encode_announcement(const Announcement& announcement) {
    if (announcement.session_name.size() > kMaxNameInAnnouncement || announcement.host_name.size() > kMaxNameInAnnouncement)
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::TooLarge, "nom trop long pour une annonce", "discovery"});
    if (!valid_text(announcement.session_name) || !valid_text(announcement.host_name))
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::InvalidArgument, "nom non UTF-8", "discovery"});
    if (announcement.control_port == 0)
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::InvalidArgument, "port de contrôle nul", "discovery"});
    std::vector<std::uint8_t> out;
    out.reserve(96);
    for (const auto byte : kMagic) out.push_back(static_cast<std::uint8_t>(byte));
    put(out, kAnnounceVersion, 2);
    put(out, announcement.sequence, 4);
    put(out, announcement.session_id, 8);
    put(out, announcement.control_port, 2);
    put(out, announcement.participants, 2);
    put(out, announcement.max_participants, 2);
    put(out, announcement.shared_services, 1);
    put(out, announcement.accepting ? 1 : 0, 1);
    put(out, announcement.wall_ms, 8);
    put(out, announcement.session_name.size(), 2);
    out.insert(out.end(), announcement.session_name.begin(), announcement.session_name.end());
    put(out, announcement.host_name.size(), 2);
    out.insert(out.end(), announcement.host_name.begin(), announcement.host_name.end());
    if (out.size() > kMaxAnnouncementBytes)
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::TooLarge, "annonce au-delà de 512 octets", "discovery"});
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}

Result<Announcement> decode_announcement(std::span<const std::uint8_t> datagram) {
    // Borne minimale exacte : 8 octets de magie + 30 octets de champs fixes +
    // 2 longueurs de nom (session et hôte). Une borne plus petite laissait les
    // lectures de longueur déborder du datagramme tronqué.
    constexpr std::size_t kMinimumAnnouncementBytes = 8 + 30 + 2 + 2;
    if (datagram.size() < kMinimumAnnouncementBytes || datagram.size() > kMaxAnnouncementBytes)
        return Result<Announcement>::failure(malformed());
    for (std::size_t index = 0; index < kMagic.size(); ++index)
        if (datagram[index] != static_cast<std::uint8_t>(kMagic[index])) return Result<Announcement>::failure(malformed());
    std::size_t offset = kMagic.size();
    Announcement announcement;
    if (take(datagram, offset, 2) != kAnnounceVersion)
        return Result<Announcement>::failure({ErrorCode::UnsupportedVersion, "version d'annonce inconnue", "discovery"});
    announcement.sequence = static_cast<std::uint32_t>(take(datagram, offset, 4));
    announcement.session_id = take(datagram, offset, 8);
    announcement.control_port = static_cast<std::uint16_t>(take(datagram, offset, 2));
    announcement.participants = static_cast<std::uint16_t>(take(datagram, offset, 2));
    announcement.max_participants = static_cast<std::uint16_t>(take(datagram, offset, 2));
    announcement.shared_services = static_cast<std::uint8_t>(take(datagram, offset, 1));
    const auto accepting = take(datagram, offset, 1);
    if (accepting > 1) return Result<Announcement>::failure(malformed());
    announcement.accepting = accepting == 1;
    announcement.wall_ms = take(datagram, offset, 8);
    const auto name_size = static_cast<std::size_t>(take(datagram, offset, 2));
    if (name_size > kMaxNameInAnnouncement || datagram.size() - offset < name_size) return Result<Announcement>::failure(malformed());
    announcement.session_name.assign(reinterpret_cast<const char*>(datagram.data() + offset), name_size);
    offset += name_size;
    if (datagram.size() - offset < 2) return Result<Announcement>::failure(malformed());
    const auto host_size = static_cast<std::size_t>(take(datagram, offset, 2));
    // La longueur vient d'être consommée : il reste exactement host_size octets.
    if (host_size > kMaxNameInAnnouncement || datagram.size() - offset < host_size) return Result<Announcement>::failure(malformed());
    announcement.host_name.assign(reinterpret_cast<const char*>(datagram.data() + offset), host_size);
    offset += host_size;
    if (offset != datagram.size()) return Result<Announcement>::failure(malformed()); // aucun octet surnuméraire.
    if (!valid_text(announcement.session_name) || !valid_text(announcement.host_name)) return Result<Announcement>::failure(malformed());
    if (announcement.session_id == 0 || announcement.control_port == 0 || announcement.max_participants == 0)
        return Result<Announcement>::failure(malformed());
    for (const auto character : announcement.session_name)
        if (static_cast<unsigned char>(character) < 0x20) return Result<Announcement>::failure(malformed());
    for (const auto character : announcement.host_name)
        if (static_cast<unsigned char>(character) < 0x20) return Result<Announcement>::failure(malformed());
    return Result<Announcement>::success(std::move(announcement));
}

Announcer::Config announce_targets(const std::vector<NetworkInterfaceInfo>& interfaces, const Announcement& announcement,
    Port discovery_port, bool include_loopback) {
    Announcer::Config config;
    config.announcement = announcement;
    const auto group = IpAddress::parse(kDiscoveryGroup);
    if (!group) return config; // aucune cible : l'appelant verra sent_datagrams() rester à 0.
    const auto chosen = select_lan_interfaces(interfaces);
    for (const auto index : chosen) {
        const auto& info = interfaces[index];
        if (auto address = IpAddress::parse(info.address); address && address.value().family == IpFamily::Ipv4) {
            config.interfaces.push_back(address.value());
            config.extra_targets.emplace_back(group.value(), discovery_port);
        }
    }
    if (include_loopback) config.extra_targets.emplace_back(IpAddress::loopback(IpFamily::Ipv4), discovery_port);
    return config;
}

// ---------------------------------------------------------------------------
// DiscoveryListener
// ---------------------------------------------------------------------------

DiscoveryListener::~DiscoveryListener() { stop(); }

Result<void> DiscoveryListener::start(const Config& config) {
    if (running_.load()) return Result<void>::failure({ErrorCode::InvalidState, "écoute déjà démarrée", "discovery"});
    config_ = config;
    auto created = Socket::create(TransportProtocol::Udp, config_.bind.family);
    if (!created) return Result<void>::failure(created.error());
    socket_ = std::move(created).value();
    if (auto reuse = socket_.set_address_reuse(true); !reuse) return Result<void>::failure(reuse.error());
    if (config_.allow_broadcast && config_.bind.family == IpFamily::Ipv4) {
        if (auto broadcast = socket_.set_broadcast(true); !broadcast) return Result<void>::failure(broadcast.error());
    }
    if (auto bound = socket_.bind(config_.bind, config_.port); !bound) return Result<void>::failure(bound.error());
    auto port = socket_.bound_port();
    if (!port) return Result<void>::failure(port.error());
    bound_port_ = port.value();
    if (auto nonblocking = socket_.set_nonblocking(true); !nonblocking) return Result<void>::failure(nonblocking.error());
    if (auto signal = wakeup_.open(); !signal) return Result<void>::failure(signal.error());
    {
        std::lock_guard<std::mutex> guard(mutex_);
        warnings_.clear();
        sessions_.clear();
        view_ = std::make_shared<const std::vector<DiscoveredSession>>();
    }
    if (config_.join_multicast) {
        // Chaque interface LAN rejoint le groupe ; un échec est un avertissement
        // visible, jamais un démarrage silencieux en mode dégradé inconnu.
        auto group = IpAddress::parse(kDiscoveryGroup);
        if (!group) return Result<void>::failure(group.error());
        bool joined_any = false;
        if (auto joined = socket_.join_multicast(group.value(), IpAddress{}); joined) joined_any = true;
        else {
            std::lock_guard<std::mutex> guard(mutex_);
            warnings_.push_back({ErrorCode::PlatformError, "multicast sur l'interface par défaut indisponible", "discovery", joined.error().native_code});
        }
        if (!joined_any) {
            std::lock_guard<std::mutex> guard(mutex_);
            warnings_.push_back({ErrorCode::Unsupported, "aucune adhésion multicast : découverte limitée au broadcast et à la saisie manuelle", "discovery"});
        }
    }
    running_.store(true);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return Result<void>::success();
}

void DiscoveryListener::stop() {
    if (!running_.exchange(false)) {
        if (worker_.joinable()) { wakeup_.notify(); worker_.request_stop(); worker_.join(); }
        socket_.close();
        return;
    }
    wakeup_.notify();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    socket_.close();
}

void DiscoveryListener::publish() {
    const auto now = clock_.now_monotonic_ms();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->expired(now)) { it = sessions_.erase(it); ++revision_; } else ++it;
    }
    if (!view_ || revision_ != published_revision_) {
        view_ = std::make_shared<const std::vector<DiscoveredSession>>(sessions_.begin(), sessions_.end());
        published_revision_ = revision_;
    }
}

void DiscoveryListener::run(std::stop_token token) {
    std::array<std::uint8_t, kMaxAnnouncementBytes> buffer{};
    while (!token.stop_requested() && running_.load()) {
        std::array<PollEntry, 2> entries{PollEntry{socket_.native(), true, false, false, false, false},
            PollEntry{wakeup_.native(), true, false, false, false, false}};
        auto waited = poll_sockets(entries, std::chrono::milliseconds(200));
        if (!waited) {
            std::lock_guard<std::mutex> guard(mutex_);
            warnings_.push_back(waited.error());
            publish();
            continue;
        }
        if (entries[1].readable) { wakeup_.drain(); continue; }
        if (!entries[0].readable) {
            std::lock_guard<std::mutex> guard(mutex_);
            publish(); // élagage périodique des annonces disparues.
            changed_.notify_all();
            continue;
        }
        for (int datagram = 0; datagram < 16; ++datagram) {
            IpAddress source;
            Port source_port{};
            auto received = socket_.receive_from(buffer, &source, &source_port);
            if (!received) {
                if (received.error().code == ErrorCode::WouldBlock) break;
                rejected_.fetch_add(1);
                break;
            }
            if (received.value() == 0) break;
            auto decoded = decode_announcement({buffer.data(), received.value()});
            if (!decoded) { rejected_.fetch_add(1); continue; }
            const auto& announcement = decoded.value();
            const auto now = clock_.now_monotonic_ms();
            std::lock_guard<std::mutex> guard(mutex_);
            DiscoveredSession candidate;
            candidate.session_id = announcement.session_id;
            candidate.session_name = announcement.session_name;
            candidate.host_name = announcement.host_name;
            candidate.address = source; // l'adresse réellement observée fait foi.
            candidate.control_port = announcement.control_port;
            candidate.participants = announcement.participants;
            candidate.max_participants = announcement.max_participants;
            candidate.shared_services = announcement.shared_services;
            candidate.accepting = announcement.accepting;
            candidate.sequence = announcement.sequence;
            candidate.last_seen_ms = now;
            const auto existing = std::find_if(sessions_.begin(), sessions_.end(),
                [&](const DiscoveredSession& item) { return same_session(item, candidate); });
            if (existing == sessions_.end()) {
                candidate.first_seen_ms = now;
                if (sessions_.size() >= 64) { rejected_.fetch_add(1); continue; } // borne stricte, jamais illimitée.
                sessions_.push_back(std::move(candidate));
                ++revision_;
            } else if (existing->sequence <= candidate.sequence || existing->manual) {
                candidate.first_seen_ms = existing->first_seen_ms;
                *existing = std::move(candidate); // une annonce réelle remplace une saisie manuelle.
                ++revision_;
            }
            accepted_.fetch_add(1);
            publish();
            changed_.notify_all();
        }
    }
    std::lock_guard<std::mutex> guard(mutex_);
    publish();
}

std::shared_ptr<const std::vector<DiscoveredSession>> DiscoveryListener::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return view_ ? view_ : std::make_shared<const std::vector<DiscoveredSession>>();
}

std::vector<Error> DiscoveryListener::warnings() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return warnings_;
}

void DiscoveryListener::inject_manual(IpAddress address, Port control_port, std::string session_name) {
    if (!address.valid() || control_port.value == 0) return;
    const auto now = clock_.now_monotonic_ms();
    DiscoveredSession manual;
    manual.session_id = 0; // inconnue : seule la tentative de connexion prouvera la session.
    manual.session_name = session_name.empty() ? std::string("Saisie manuelle") : std::move(session_name);
    manual.host_name = address.text;
    manual.address = std::move(address);
    manual.control_port = control_port.value;
    manual.accepting = true;
    manual.manual = true;
    manual.first_seen_ms = now;
    manual.last_seen_ms = now;
    std::lock_guard<std::mutex> guard(mutex_);
    const auto existing = std::find_if(sessions_.begin(), sessions_.end(),
        [&](const DiscoveredSession& item) { return same_session(item, manual); });
    if (existing == sessions_.end()) {
        if (sessions_.size() >= 64) return;
        sessions_.push_back(std::move(manual));
    } else {
        manual.first_seen_ms = existing->first_seen_ms;
        *existing = std::move(manual);
    }
    ++revision_;
    publish();
    changed_.notify_all();
}

bool DiscoveryListener::wait_for_sessions(std::size_t minimum, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(mutex_);
    return changed_.wait_for(guard, timeout, [&] { return sessions_.size() >= minimum; });
}

// ---------------------------------------------------------------------------
// Announcer
// ---------------------------------------------------------------------------

Announcer::~Announcer() { stop(); }

Result<void> Announcer::prepare() {
    auto created = Socket::create(TransportProtocol::Udp, config_.source.family);
    if (!created) return Result<void>::failure(created.error());
    socket_ = std::move(created).value();
    if (auto reuse = socket_.set_address_reuse(true); !reuse) return Result<void>::failure(reuse.error());
    if (auto broadcast = socket_.set_broadcast(true); !broadcast) return Result<void>::failure(broadcast.error());
    if (auto bound = socket_.bind(config_.source, Port{0}); !bound) return Result<void>::failure(bound.error());
    if (auto nonblocking = socket_.set_nonblocking(false); !nonblocking) return Result<void>::failure(nonblocking.error());
    if (auto ttl = socket_.set_multicast_ttl(config_.ttl); !ttl) {
        std::lock_guard<std::mutex> guard(mutex_);
        failures_.push_back(ttl.error());
    }
    if (auto loop = socket_.set_multicast_loop(config_.multicast_loop); !loop) {
        std::lock_guard<std::mutex> guard(mutex_);
        failures_.push_back(loop.error());
    }
    targets_.clear();
    for (const auto& target : config_.extra_targets) targets_.push_back(target);
    return Result<void>::success();
}

Result<void> Announcer::start(Config config) {
    if (running_.load()) return Result<void>::failure({ErrorCode::InvalidState, "annonce déjà démarrée", "discovery"});
    config_ = std::move(config);
    if (config_.interval_ms < 250) config_.interval_ms = 250;
    if (auto prepared = prepare(); !prepared) return prepared;
    if (auto signal = wakeup_.open(); !signal) return Result<void>::failure(signal.error());
    {
        std::lock_guard<std::mutex> guard(mutex_);
        failures_.clear();
    }
    running_.store(true);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return Result<void>::success();
}

void Announcer::stop() {
    const bool was_running = running_.exchange(false);
    wakeup_.notify();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    if (was_running || socket_.valid()) socket_.close();
}

void Announcer::update(Announcement announcement) {
    std::lock_guard<std::mutex> guard(mutex_);
    config_.announcement = std::move(announcement);
}

Result<std::size_t> Announcer::send_now() {
    Announcement announcement;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        announcement = config_.announcement;
    }
    announcement.sequence = ++sequence_;
    announcement.wall_ms = clock_.now_wall_ms();
    auto datagram = encode_announcement(announcement);
    if (!datagram) return Result<std::size_t>::failure(datagram.error());
    std::size_t delivered = 0;
    auto group = IpAddress::parse(kDiscoveryGroup);
    if (!group) return Result<std::size_t>::failure(group.error());
    Port group_port{kDiscoveryPort};
    for (const auto& target : config_.extra_targets) {
        if (target.first.text == kDiscoveryGroup) { group_port = target.second; break; }
    }
    // Multicast : une émission par interface LAN, avec l'interface de sortie explicite.
    for (const auto& interface : config_.interfaces) {
        if (auto selected = socket_.set_multicast_interface(interface); !selected) {
            std::lock_guard<std::mutex> guard(mutex_);
            failures_.push_back(selected.error());
            continue;
        }
        auto sent = socket_.send_to(group.value(), group_port, datagram.value());
        if (sent) { delivered += sent.value(); sent_.fetch_add(1); }
        else {
            std::lock_guard<std::mutex> guard(mutex_);
            if (failures_.size() < 32) failures_.push_back(sent.error());
        }
    }
    // Cibles explicites : loopback (démo une machine), broadcast, ports de test.
    for (const auto& target : config_.extra_targets) {
        if (target.first.is_multicast()) continue; // déjà traité par interface.
        auto sent = socket_.send_to(target.first, target.second, datagram.value());
        if (sent) { delivered += sent.value(); sent_.fetch_add(1); }
        else {
            std::lock_guard<std::mutex> guard(mutex_);
            if (failures_.size() < 32) failures_.push_back(sent.error());
        }
    }
    if (delivered == 0)
        return Result<std::size_t>::failure({ErrorCode::IoError, "aucune annonce émise", "discovery"});
    return Result<std::size_t>::success(delivered);
}

void Announcer::run(std::stop_token token) {
    (void)send_now();
    while (!token.stop_requested() && running_.load()) {
        std::array<PollEntry, 1> entries{PollEntry{wakeup_.native(), true, false, false, false, false}};
        auto waited = poll_sockets(entries, std::chrono::milliseconds(config_.interval_ms));
        if (waited && waited.value() > 0 && entries[0].readable) { wakeup_.drain(); continue; }
        (void)send_now();
    }
}

std::vector<Error> Announcer::failures() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return failures_;
}

} // namespace plpt::net
