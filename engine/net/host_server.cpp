// PLPT — engine/net/host_server.cpp — plan de contrôle hôte : authentification,
// participants, messagerie, révocation, délais. Tout état de session appartient
// au thread d'entrées/sorties ; les autres threads n'écrivent que des commandes.
#include "host_server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include "../core/format.hpp"
#include "../core/random.hpp"
#include "../core/text.hpp"

namespace plpt::net {
namespace {
constexpr std::size_t kMaxConnections = 64;
constexpr std::size_t kMaxConnectionsPerAddress = 8;
constexpr std::size_t kMaxPendingJoins = 16;
constexpr std::size_t kMaxOutboxBytes = 256 * 1024;
constexpr std::size_t kMaxEvents = 200;
constexpr std::size_t kMaxWarnings = 32;
constexpr unsigned kMaxCodeFailuresPerConnection = 3;
constexpr unsigned kMaxCodeFailuresPerAddress = 8;
constexpr std::uint64_t kCodeFailureWindowMs = 10 * 60 * 1000;
constexpr std::size_t kMaxReceivePerPoll = 16;

std::string address_key(const IpAddress& address) {
    return address.text + "%" + std::to_string(address.scope_id);
}

Result<std::uint64_t> random_session_id() {
    SecureRandom random;
    std::array<std::uint8_t, 8> bytes{};
    if (!random.fill(bytes))
        return Result<std::uint64_t>::failure({ErrorCode::PlatformError, "CSPRNG indisponible pour l'identifiant de session", "host"});
    std::uint64_t value = 0;
    for (const auto byte : bytes) value = (value << 8) | byte;
    if (value == 0) value = 1; // SessionId 0 est réservé : jamais une session réelle.
    return Result<std::uint64_t>::success(value);
}

} // namespace

struct HostServer::Connection {
    Socket socket;
    IpAddress address;
    Port port{};
    FrameAssembler assembler;
    std::vector<std::uint8_t> outbox;
    std::size_t outbox_sent = 0;
    enum class Phase : std::uint8_t { AwaitingHello, AwaitingApproval, Authed, Closing } phase = Phase::AwaitingHello;
    ParticipantId participant{};
    std::uint64_t request_id = 0;
    std::string guest_name;
    std::string client;
    std::uint64_t opened_ms = 0;
    std::uint64_t last_activity_ms = 0;
    std::uint64_t next_heartbeat_ms = 0;
    std::uint64_t close_deadline_ms = 0;
    std::uint64_t sequence_in = 0;
    std::uint64_t sequence_out = 0;
    std::uint64_t heartbeat_sent_ms = 0;
    std::uint64_t heartbeat_echo = 0;
    std::uint32_t latency_ms = 0;
    unsigned code_failures = 0;
    unsigned invalid_frames = 0;
};

HostServer::HostServer(IClock& clock) : clock_(clock) {}
HostServer::~HostServer() { stop(); }

Result<std::uint16_t> HostServer::start(HostConfig config) {
    if (running_.load()) return Result<std::uint16_t>::failure({ErrorCode::InvalidState, "serveur déjà démarré", "host"});
    if (auto name = sanitize_name(config.session_name, kMaxNameBytes); !name)
        return Result<std::uint16_t>::failure(name.error());
    else config.session_name = name.value();
    if (auto host = sanitize_name(config.host_name, kMaxNameBytes); !host)
        return Result<std::uint16_t>::failure(host.error());
    else config.host_name = host.value();
    if (config.max_participants == 0 || config.max_participants > 64)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "max_participants hors 1..64", "host"});
    if (config.session_ttl_ms == 0)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "durée de session nulle", "host"});
    if (!config.invite_code.valid())
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "code d'invitation absent : aucune session ne peut être jointe", "host"});
    auto identifier = random_session_id();
    if (!identifier) return Result<std::uint16_t>::failure(identifier.error());

    config_ = std::move(config);
    session_id_ = identifier.value();
    started_ms_ = clock_.now_monotonic_ms();
    counters_ = HostCounters{};
    connections_.clear();
    pending_.clear();
    events_.clear();
    warnings_.clear();
    services_.clear();
    attempts_.clear();
    reconnect_slots_.clear();
    revision_ = 0;
    published_revision_ = 0;
    shutdown_done_ = false;

    session_ = std::make_unique<model::Session>(SessionId{session_id_},
        model::SessionPolicy{config_.max_participants, config_.session_ttl_ms}, clock_);
    messaging_ = std::make_unique<model::Messaging>(*session_);
    participant_meta_.clear();
    if (auto host_id = session_->register_host(); !host_id) return Result<std::uint16_t>::failure(host_id.error());
    else host_participant_ = host_id.value();
    if (auto step = session_->transition_to(model::SessionState::Discovering); !step) return Result<std::uint16_t>::failure(step.error());
    if (auto step = session_->transition_to(model::SessionState::AwaitingGuests); !step) return Result<std::uint16_t>::failure(step.error());
    if (auto step = session_->open_for_guests(); !step) return Result<std::uint16_t>::failure(step.error());

    auto created = Socket::create(TransportProtocol::Tcp, config_.bind_address.family);
    if (!created) return Result<std::uint16_t>::failure(created.error());
    listener_ = std::move(created).value();
    if (auto exclusive = listener_.set_exclusive_bind(true); !exclusive) return Result<std::uint16_t>::failure(exclusive.error());
    if (auto reuse = listener_.set_address_reuse(false); !reuse) return Result<std::uint16_t>::failure(reuse.error());
    if (auto bound = listener_.bind(config_.bind_address, Port{config_.control_port}); !bound)
        return Result<std::uint16_t>::failure(bound.error());
    auto port = listener_.bound_port();
    if (!port) return Result<std::uint16_t>::failure(port.error());
    control_port_ = port.value().value;
    if (auto listening = listener_.listen(32); !listening) return Result<std::uint16_t>::failure(listening.error());
    if (auto nonblocking = listener_.set_nonblocking(true); !nonblocking) return Result<std::uint16_t>::failure(nonblocking.error());
    if (auto signal = wakeup_.open(); !signal) return Result<std::uint16_t>::failure(signal.error());
    if (config_.control_address.empty()) config_.control_address = config_.bind_address.is_any() ? std::string("127.0.0.1") : config_.bind_address.text;

    record_event("session « " + config_.session_name + " » ouverte sur le port " + std::to_string(control_port_));
    running_.store(true);
    publish();
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return Result<std::uint16_t>::success(control_port_);
}

void HostServer::stop() {
    if (!running_.load() && !worker_.joinable()) {
        listener_.close();
        return;
    }
    running_.store(false);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        commands_.push_back(Command{CommandKind::Stop, 0, ParticipantId{}, "", {}});
    }
    wakeup_.notify();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    for (auto& connection : connections_) connection->socket.close();
    connections_.clear();
    listener_.close();
    publish();
}

bool HostServer::enqueue(Command command) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_.load() && command.kind != CommandKind::Stop) return false;
        if (commands_.size() >= 256) return false; // file bornée : jamais de croissance infinie.
        commands_.push_back(std::move(command));
    }
    wakeup_.notify();
    return true;
}

bool HostServer::approve(std::uint64_t request_id) {
    return enqueue(Command{CommandKind::Approve, request_id, ParticipantId{}, "", {}});
}
bool HostServer::deny(std::uint64_t request_id, std::string reason) {
    return enqueue(Command{CommandKind::Deny, request_id, ParticipantId{}, std::move(reason), {}});
}
bool HostServer::revoke(ParticipantId participant) {
    return enqueue(Command{CommandKind::Revoke, 0, participant, "", {}});
}
bool HostServer::set_services(std::vector<SharedServiceRecord> services) {
    return enqueue(Command{CommandKind::SetServices, 0, ParticipantId{}, "", std::move(services)});
}
bool HostServer::post_chat(std::string body) {
    return enqueue(Command{CommandKind::PostChat, 0, ParticipantId{}, std::move(body), {}});
}
bool HostServer::post_direct_message(ParticipantId peer, std::string body) {
    return enqueue(Command{CommandKind::PostDirect, 0, peer, std::move(body), {}});
}
bool HostServer::broadcast_event(std::string text) {
    return enqueue(Command{CommandKind::PostEvent, 0, ParticipantId{}, std::move(text), {}});
}

std::shared_ptr<const HostSnapshot> HostServer::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return view_ ? view_ : std::make_shared<const HostSnapshot>();
}

std::size_t HostServer::open_connections() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return view_ ? view_->connection_count : 0;
}

bool HostServer::wait_for(const std::function<bool(const HostSnapshot&)>& predicate, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(mutex_);
    return published_.wait_for(guard, timeout, [&] { return view_ && predicate(*view_); });
}

void HostServer::record_event(std::string text) {
    const auto stamp = format_clock_utc(clock_.now_wall_ms());
    events_.push_back(stamp + "  " + std::move(text));
    while (events_.size() > kMaxEvents) events_.pop_front();
    ++revision_;
}

void HostServer::run(std::stop_token token) {
    std::array<std::uint8_t, 8192> buffer{};
    while (true) {
        // Les commandes sont drainées AVANT tout test d'arrêt : sinon un arrêt
        // demandé depuis un autre thread ferait sortir de la boucle sans que la
        // fermeture réelle (Bye, transitions de session) ait eu lieu.
        drain_commands();
        if (!running_.load() || token.stop_requested()) break;
        connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
            [](const std::unique_ptr<Connection>& connection) { return !connection->socket.valid(); }), connections_.end());
        accept_pending_connections();
        std::vector<PollEntry> entries;
        entries.reserve(connections_.size() + 2);
        entries.push_back(PollEntry{listener_.native(), true, false, false, false, false});
        entries.push_back(PollEntry{wakeup_.native(), true, false, false, false, false});
        for (const auto& connection : connections_) {
            const bool wants_write = connection->outbox_sent < connection->outbox.size();
            entries.push_back(PollEntry{connection->socket.native(), connection->phase != Connection::Phase::Closing, wants_write, false, false, false});
        }
        std::uint32_t wait_ms = 100;
        const auto now = clock_.now_monotonic_ms();
        for (const auto& connection : connections_) {
            if (connection->phase == Connection::Phase::Authed && connection->next_heartbeat_ms > now) {
                const auto until = static_cast<std::uint32_t>(std::min<std::uint64_t>(connection->next_heartbeat_ms - now, wait_ms));
                wait_ms = std::min(wait_ms, until == 0 ? 1U : until);
            }
        }
        const auto waited = poll_sockets(entries, std::chrono::milliseconds(wait_ms));
        if (!waited) {
            if (warnings_.size() < kMaxWarnings) warnings_.push_back(waited.error());
            ++revision_;
        } else {
            if (entries[0].readable) accept_pending_connections();
            if (entries[1].readable) { wakeup_.drain(); continue; }
            for (std::size_t index = 0; index < connections_.size(); ++index) {
                auto& connection = *connections_[index];
                const auto& entry = entries[index + 2];
                if (entry.failed) {
                    disconnect(connection, ErrorCode::Disconnected, "connexion interrompue", false);
                    continue;
                }
                if (entry.readable && connection.phase != Connection::Phase::Closing) {
                    for (std::size_t round = 0; round < kMaxReceivePerPoll; ++round) {
                        auto received = connection.socket.receive(buffer);
                        if (!received) {
                            if (received.error().code == ErrorCode::WouldBlock) break;
                            disconnect(connection, received.error().code, "lecture impossible", false);
                            break;
                        }
                        if (received.value() == 0) break;
                        connection.last_activity_ms = clock_.now_monotonic_ms();
                        if (auto fed = connection.assembler.feed({buffer.data(), received.value()}); !fed) {
                            counters_.invalid_frames++;
                            disconnect(connection, fed.error().code, "flux de contrôle invalide", true);
                            break;
                        }
                        bool progress = true;
                        while (progress) {
                            auto frame = connection.assembler.next();
                            if (!frame) {
                                progress = false;
                                if (frame.error().code != ErrorCode::WouldBlock) {
                                    counters_.invalid_frames++;
                                    connection.invalid_frames++;
                                    disconnect(connection, frame.error().code, "trame invalide", true);
                                }
                                break;
                            }
                            handle_frame(connection, frame.value());
                            if (connection.phase == Connection::Phase::Closing) { progress = false; break; }
                        }
                    }
                }
                if (connection.phase == Connection::Phase::Closing && connection.outbox_sent >= connection.outbox.size()) {
                    connection.socket.close();
                    continue;
                }
                if (entry.writable || entry.readable) {
                    while (connection.outbox_sent < connection.outbox.size()) {
                        auto sent = connection.socket.send({connection.outbox.data() + connection.outbox_sent,
                            connection.outbox.size() - connection.outbox_sent});
                        if (!sent) break; // WouldBlock : le reste part au prochain tour.
                        if (sent.value() == 0) break;
                        connection.outbox_sent += sent.value();
                    }
                    if (connection.outbox_sent == connection.outbox.size()) {
                        connection.outbox.clear();
                        connection.outbox_sent = 0;
                    } else if (connection.outbox_sent > 0) {
                        connection.outbox.erase(connection.outbox.begin(),
                            connection.outbox.begin() + static_cast<std::ptrdiff_t>(connection.outbox_sent));
                        connection.outbox_sent = 0;
                    }
                }
            }
            connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                [](const std::unique_ptr<Connection>& connection) { return !connection->socket.valid(); }), connections_.end());
        }
        apply_timeouts();
        if (revision_ != published_revision_) publish();
    }
    shutdown();
    publish();
}

void HostServer::shutdown() {
    if (shutdown_done_) return;
    shutdown_done_ = true;
    const auto now = clock_.now_monotonic_ms();
    for (auto& connection : connections_) {
        if (connection->phase == Connection::Phase::Authed || connection->phase == Connection::Phase::AwaitingApproval)
            notify_refusal(*connection, ErrorCode::Cancelled, "l'hôte a arrêté la session");
        connection->phase = Connection::Phase::Closing;
        connection->close_deadline_ms = now + 250;
    }
    flush_bounded(std::chrono::milliseconds(300));
    for (auto& connection : connections_) {
        if (connection->participant.value != 0 && session_)
            (void)session_->set_participant_state(connection->participant, model::ParticipantState::Disconnected);
        connection->socket.close();
    }
    connections_.clear();
    listener_.close();
    // La session ne peut rester « active » après un arrêt : les transitions sont
    // tentées dans l'ordre et leur échec est journalisé, jamais masqué.
    if (session_) {
        if (auto step = session_->transition_to(model::SessionState::Closing); !step) warnings_.push_back(step.error());
        if (session_->state() == model::SessionState::Closing) {
            if (auto step = session_->transition_to(model::SessionState::Cleanup); !step) warnings_.push_back(step.error());
        }
        if (session_->state() == model::SessionState::Cleanup) {
            CleanupReport report;
            report.complete = true;
            report.removed_profiles = 0;
            if (auto step = session_->complete_cleanup(report); !step) warnings_.push_back(step.error());
        }
        if (session_->state() != model::SessionState::Closed)
            record_event("session non fermée complètement : état " + std::string(session_state_text(session_->state())));
    }
    record_event("session fermée, sockets libérées");
}

void HostServer::drain_commands() {
    std::deque<Command> pending;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pending.swap(commands_);
    }
    for (const auto& command : pending) {
        switch (command.kind) {
        case CommandKind::Approve: approve_request(command.request_id); break;
        case CommandKind::Deny: deny_request(command.request_id, command.text); break;
        case CommandKind::Revoke: revoke_participant(command.participant); break;
        case CommandKind::SetServices:
            services_ = command.services;
            record_event("services partagés mis à jour (" + std::to_string(services_.size()) + ")");
            broadcast_service_list();
            ++revision_;
            break;
        case CommandKind::PostChat: {
            auto body = sanitize_chat_body(command.text);
            if (!body) {
                if (warnings_.size() < kMaxWarnings) warnings_.push_back(body.error());
                ++revision_;
                break;
            }
            auto posted = messaging_->post_general(host_participant_, body.value());
            if (!posted) {
                if (warnings_.size() < kMaxWarnings) warnings_.push_back(posted.error());
                ++revision_;
                break;
            }
            model::ChatMessage line;
            line.id = posted.value();
            line.sender = host_participant_;
            line.body = body.value();
            line.sent_ms = clock_.now_monotonic_ms();
            relay_chat(line, config_.host_name, false, ParticipantId{});
            counters_.chat_relayed++;
            ++revision_;
            break;
        }
        case CommandKind::PostDirect: {
            auto body = sanitize_chat_body(command.text);
            if (!body) {
                if (warnings_.size() < kMaxWarnings) warnings_.push_back(body.error());
                ++revision_;
                break;
            }
            auto posted = messaging_->post_dm(host_participant_, command.participant, body.value());
            if (!posted) {
                if (warnings_.size() < kMaxWarnings) warnings_.push_back(posted.error());
                ++revision_;
                break;
            }
            model::ChatMessage line;
            line.id = posted.value();
            line.sender = host_participant_;
            line.recipient = command.participant;
            line.body = body.value();
            line.sent_ms = clock_.now_monotonic_ms();
            relay_chat(line, config_.host_name, true, command.participant);
            counters_.chat_relayed++;
            ++revision_;
            break;
        }
        case CommandKind::PostEvent: {
            record_event(command.text);
            ChatText notice;
            notice.sender = host_participant_;
            notice.sender_name = config_.host_name;
            notice.body = command.text;
            notice.sent_ms = clock_.now_wall_ms();
            notice.system = true;
            broadcast(model::MessageType::Chat, encode(notice), ParticipantId{});
            break;
        }
        case CommandKind::Stop:
            running_.store(false);
            record_event("arrêt de la session demandé par l'hôte");
            break;
        }
    }
}

void HostServer::flush_bounded(std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        bool pending = false;
        std::vector<PollEntry> entries;
        entries.reserve(connections_.size());
        for (auto& connection : connections_) {
            if (connection->outbox_sent >= connection->outbox.size()) continue;
            pending = true;
            entries.push_back(PollEntry{connection->socket.native(), false, true, false, false, false});
        }
        if (!pending) return;
        auto waited = poll_sockets(entries, std::chrono::milliseconds(50));
        if (!waited) return;
        std::size_t index = 0;
        for (auto& connection : connections_) {
            if (connection->outbox_sent >= connection->outbox.size()) continue;
            if (index < entries.size() && entries[index].writable) {
                auto sent = connection->socket.send({connection->outbox.data() + connection->outbox_sent,
                    connection->outbox.size() - connection->outbox_sent});
                if (sent) connection->outbox_sent += sent.value();
            }
            ++index;
        }
    }
}

bool HostServer::rate_limited(const IpAddress& address) {
    const auto key = address_key(address);
    const auto now = clock_.now_monotonic_ms();
    const auto entry = std::find_if(attempts_.begin(), attempts_.end(), [&](const auto& item) { return item.first == key; });
    if (entry == attempts_.end()) return false;
    auto& attempt = entry->second;
    if (now - attempt.window_start_ms > kCodeFailureWindowMs) attempt = AddressAttempt{};
    return attempt.failures >= kMaxCodeFailuresPerAddress || attempt.connections >= kMaxConnectionsPerAddress;
}

void HostServer::record_code_failure(const IpAddress& address) {
    const auto key = address_key(address);
    const auto now = clock_.now_monotonic_ms();
    auto entry = std::find_if(attempts_.begin(), attempts_.end(), [&](const auto& item) { return item.first == key; });
    if (entry == attempts_.end()) {
        if (attempts_.size() >= 256) attempts_.clear(); // borne stricte, pas d'historique illimité.
        attempts_.emplace_back(key, AddressAttempt{now, 1, 1});
        return;
    }
    if (now - entry->second.window_start_ms > kCodeFailureWindowMs) entry->second = AddressAttempt{now, 0, 0};
    entry->second.failures++;
}

void HostServer::accept_pending_connections() {
    while (connections_.size() < kMaxConnections) {
        IpAddress peer;
        Port peer_port{};
        auto accepted = listener_.accept(&peer, &peer_port);
        if (!accepted) {
            if (accepted.error().code != ErrorCode::WouldBlock && warnings_.size() < kMaxWarnings) {
                warnings_.push_back(accepted.error());
                ++revision_;
            }
            return;
        }
        auto connection = std::make_unique<Connection>();
        connection->socket = std::move(accepted).value();
        connection->address = peer;
        connection->port = peer_port;
        const auto now = clock_.now_monotonic_ms();
        connection->opened_ms = now;
        connection->last_activity_ms = now;
        connection->next_heartbeat_ms = now + config_.heartbeat_interval_ms;
        (void)connection->socket.set_nonblocking(true);
        (void)connection->socket.set_no_delay(true);
        const auto key = address_key(peer);
        auto entry = std::find_if(attempts_.begin(), attempts_.end(), [&](const auto& item) { return item.first == key; });
        if (entry == attempts_.end()) {
            if (attempts_.size() >= 256) attempts_.clear();
            attempts_.emplace_back(key, AddressAttempt{now, 0, 1});
        } else {
            if (now - entry->second.window_start_ms > kCodeFailureWindowMs) entry->second = AddressAttempt{now, 0, 0};
            entry->second.connections++;
        }
        if (rate_limited(peer)) {
            counters_.rate_limited++;
            record_event("connexion refusée (limite atteinte) depuis " + peer.text);
            notify_refusal(*connection, ErrorCode::RateLimited, "trop de tentatives depuis cette adresse");
            connection->phase = Connection::Phase::Closing;
            connection->close_deadline_ms = now + 250;
            connections_.push_back(std::move(connection));
            ++revision_;
            continue;
        }
        counters_.connections_total++;
        connections_.push_back(std::move(connection));
        ++revision_;
    }
}

bool HostServer::queue_frame(Connection& connection, model::MessageType type, std::span<const std::uint8_t> payload) {
    if (connection.outbox.size() - connection.outbox_sent + payload.size() > kMaxOutboxBytes) {
        counters_.rate_limited++;
        connection.phase = Connection::Phase::Closing;
        connection.close_deadline_ms = clock_.now_monotonic_ms();
        return false;
    }
    auto framed = frame(type, SessionId{session_id_}, connection.participant, ++connection.sequence_out, payload);
    if (!framed) {
        if (warnings_.size() < kMaxWarnings) warnings_.push_back(framed.error());
        ++revision_;
        return false;
    }
    connection.outbox.insert(connection.outbox.end(), framed.value().begin(), framed.value().end());
    ++revision_;
    return true;
}

bool HostServer::queue_to(ParticipantId participant, model::MessageType type, std::span<const std::uint8_t> payload) {
    for (auto& connection : connections_) {
        if (connection->participant == participant && connection->phase == Connection::Phase::Authed)
            return queue_frame(*connection, type, payload);
    }
    return false;
}

void HostServer::broadcast(model::MessageType type, const std::vector<std::uint8_t>& payload, ParticipantId except) {
    for (auto& connection : connections_) {
        if (connection->phase != Connection::Phase::Authed) continue;
        if (except.value != 0 && connection->participant == except) continue;
        queue_frame(*connection, type, payload);
    }
}

SessionInfo HostServer::build_session_info() const {
    SessionInfo info;
    info.session = SessionId{session_id_};
    info.session_name = config_.session_name;
    info.host_name = config_.host_name;
    info.state = session_ ? static_cast<std::uint8_t>(session_->state()) : 0;
    info.max_participants = static_cast<std::uint32_t>(config_.max_participants);
    info.started_ms = started_ms_;
    for (const auto& meta : participant_meta_) {
        const auto* participant = session_ ? session_->find_participant(meta.first) : nullptr;
        if (!participant || participant->role == Role::Host) continue;
        PeerInfo peer;
        peer.id = meta.first;
        peer.name = meta.second.name;
        peer.state = static_cast<std::uint8_t>(participant->state);
        peer.connected_ms = meta.second.connected_ms;
        peer.latency_ms = meta.second.latency_ms;
        info.peers.push_back(std::move(peer));
        if (info.peers.size() >= kMaxPeers) break;
    }
    return info;
}

ServiceList HostServer::build_service_list() const {
    ServiceList list;
    list.control_address = config_.control_address + ":" + std::to_string(control_port_);
    for (const auto& service : services_) {
        if (service.token.empty() || service.exposed_port == 0) continue; // non publié tant que non autorisé.
        ServiceInfo info;
        info.key = service.key;
        info.label = service.label;
        info.exposed_port = service.exposed_port;
        info.url = service.url;
        info.available = service.available && service.upstream_alive;
        list.services.push_back(std::move(info));
        if (list.services.size() >= kMaxServices) break;
    }
    return list;
}

void HostServer::broadcast_session_state() {
    const auto info = build_session_info();
    for (auto& connection : connections_) {
        if (connection->phase != Connection::Phase::Authed) continue;
        // Chaque invité reçoit sa propre liste avec son marqueur « soi ».
        auto tailored = info;
        for (auto& peer : tailored.peers) peer.self = peer.id == connection->participant;
        queue_frame(*connection, model::MessageType::SessionState, encode(tailored));
    }
    ++revision_;
}

void HostServer::broadcast_service_list() {
    const auto list = build_service_list();
    const auto payload = encode(list);
    broadcast(model::MessageType::ServiceList, payload, ParticipantId{});
}

void HostServer::relay_chat(const model::ChatMessage& message, const std::string& sender_name, bool direct, ParticipantId only) {
    ChatText text;
    text.sender = message.sender;
    text.sender_name = sender_name;
    text.body = message.body;
    text.sent_ms = clock_.now_wall_ms();
    text.system = false;
    text.recipient = message.recipient.value_or(ParticipantId{}); // 0 = canal général.
    const auto payload = encode(text);
    if (direct && only.value != 0) {
        queue_to(only, model::MessageType::Chat, payload);
        return;
    }
    // Écho inclus : l'invité n'affiche que ce qu'il a réellement reçu du réseau.
    broadcast(model::MessageType::Chat, payload, ParticipantId{});
}

std::vector<ChatLine> HostServer::build_chat() const {
    std::vector<ChatLine> lines;
    if (!messaging_ || !session_) return lines;
    auto general = messaging_->general_channel(host_participant_);
    if (!general) return lines;
    for (const auto& message : general.value()) {
        ChatLine line;
        line.id = message.id.value;
        line.sender = message.sender;
        line.body = message.body;
        line.sent_ms = message.sent_ms;
        const auto meta = participant_meta_.find(message.sender);
        line.sender_name = message.sender == host_participant_ ? config_.host_name : (meta == participant_meta_.end() ? std::string("invité") : meta->second.name);
        lines.push_back(std::move(line));
    }
    for (const auto& channel : direct_channels_) {
        const auto& messages = channel.second;
        for (const auto& message : messages) {
            ChatLine line;
            line.id = message.id.value;
            line.sender = message.sender;
            line.body = message.body;
            line.sent_ms = message.sent_ms;
            line.direct = true;
            const auto meta = participant_meta_.find(message.sender);
            line.sender_name = message.sender == host_participant_ ? config_.host_name : (meta == participant_meta_.end() ? std::string("invité") : meta->second.name);
            lines.push_back(std::move(line));
        }
    }
    std::stable_sort(lines.begin(), lines.end(), [](const ChatLine& left, const ChatLine& right) { return left.id < right.id; });
    if (lines.size() > 200) lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - 200));
    return lines;
}

void HostServer::handle_frame(Connection& connection, const model::Envelope& envelope) {
    if (connection.phase == Connection::Phase::AwaitingHello) {
        if (envelope.type != model::MessageType::Hello) {
            counters_.invalid_frames++;
            disconnect(connection, ErrorCode::ProtocolViolation, "première trame attendue : Hello", true);
            return;
        }
        handle_hello(connection, envelope);
        return;
    }
    if (envelope.session.value != session_id_) {
        counters_.invalid_frames++;
        disconnect(connection, ErrorCode::ProtocolViolation, "identifiant de session incohérent", true);
        return;
    }
    if (envelope.participant != connection.participant) {
        counters_.invalid_frames++;
        disconnect(connection, ErrorCode::PermissionDenied, "identifiant de participant usurpé", true);
        return;
    }
    if (envelope.sequence <= connection.sequence_in) {
        counters_.invalid_frames++;
        disconnect(connection, ErrorCode::Replayed, "séquence rejouée ou désordonnée", true);
        return;
    }
    connection.sequence_in = envelope.sequence;
    if (connection.phase == Connection::Phase::AwaitingApproval) {
        if (envelope.type == model::MessageType::Heartbeat) { handle_heartbeat(connection, envelope); return; }
        if (envelope.type == model::MessageType::Bye) { handle_bye(connection, envelope); return; }
        counters_.invalid_frames++;
        disconnect(connection, ErrorCode::ProtocolViolation, "message refusé avant approbation", true);
        return;
    }
    if (connection.phase != Connection::Phase::Authed) return;
    switch (envelope.type) {
    case model::MessageType::Chat: handle_chat(connection, envelope); break;
    case model::MessageType::Heartbeat: handle_heartbeat(connection, envelope); break;
    case model::MessageType::Bye: handle_bye(connection, envelope); break;
    default:
        counters_.invalid_frames++;
        disconnect(connection, ErrorCode::ProtocolViolation, "type de message non autorisé sur le fil de contrôle", true);
        break;
    }
}

void HostServer::handle_hello(Connection& connection, const model::Envelope& envelope) {
    auto hello = decode_guest_hello(envelope.payload);
    if (!hello) {
        counters_.invalid_frames++;
        disconnect(connection, hello.error().code, "demande de connexion invalide", true);
        return;
    }
    auto name = sanitize_name(hello.value().guest_name, kMaxNameBytes);
    if (!name) {
        counters_.invalid_frames++;
        disconnect(connection, name.error().code, "nom d'invité refusé", true);
        return;
    }
    connection.guest_name = name.value();
    connection.client = hello.value().client;
    if (!invite_code_matches(hello.value().invite_code, config_.invite_code)) {
        counters_.bad_codes++;
        connection.code_failures++;
        record_code_failure(connection.address);
        record_event("code d'invitation refusé pour « " + connection.guest_name + " » depuis " + connection.address.text);
        if (connection.code_failures >= kMaxCodeFailuresPerConnection || rate_limited(connection.address)) {
            disconnect(connection, ErrorCode::Unauthorized, "code d'invitation incorrect", true);
            return;
        }
        HostAuth denied;
        denied.accepted = false;
        denied.reason = ErrorCode::Unauthorized;
        denied.reason_text = "code d'invitation incorrect";
        denied.host_name = config_.host_name;
        denied.session_name = config_.session_name;
        denied.approval_required = config_.approval_required;
        queue_frame(connection, model::MessageType::Auth, encode(denied));
        ++revision_;
        return;
    }
    if (config_.approval_required) {
        if (pending_.size() >= kMaxPendingJoins) {
            counters_.denied++;
            disconnect(connection, ErrorCode::CapacityExceeded, "trop de demandes en attente", true);
            return;
        }
        PendingJoin join;
        join.request_id = next_request_id_++;
        join.guest_name = connection.guest_name;
        join.address = connection.address.text;
        join.port = connection.port.value;
        join.requested_ms = clock_.now_wall_ms();
        join.client = connection.client;
        connection.request_id = join.request_id;
        connection.phase = Connection::Phase::AwaitingApproval;
        pending_.push_back(std::move(join));
        record_event("demande de connexion de « " + connection.guest_name + " » (" + connection.address.text + ")");
        HostAuth waiting;
        waiting.accepted = false;
        waiting.awaiting_approval = true;
        waiting.reason = ErrorCode::Ok;
        waiting.reason_text = "code valide : en attente d'autorisation de l'hôte";
        waiting.host_name = config_.host_name;
        waiting.session_name = config_.session_name;
        waiting.approval_required = true;
        queue_frame(connection, model::MessageType::Auth, encode(waiting));
        ChatText notice;
        notice.sender = host_participant_;
        notice.sender_name = config_.host_name;
        notice.body = connection.guest_name + " demande à rejoindre la session";
        notice.sent_ms = clock_.now_wall_ms();
        notice.system = true;
        broadcast(model::MessageType::Chat, encode(notice), ParticipantId{});
        ++revision_;
        publish();
        return;
    }
    admit(connection);
}

void HostServer::admit(Connection& connection) {
    // Reconnexion contrôlée : même nom dans le délai de grâce = même place,
    // sinon le nombre de participants croirait à chaque coupure réseau.
    const auto now = clock_.now_monotonic_ms();
    for (auto slot = reconnect_slots_.begin(); slot != reconnect_slots_.end();) {
        if (now - slot->disconnected_ms > config_.reconnect_grace_ms) slot = reconnect_slots_.erase(slot);
        else ++slot;
    }
    const auto reusable = std::find_if(reconnect_slots_.begin(), reconnect_slots_.end(),
        [&](const ReconnectSlot& slot) { return slot.name == connection.guest_name; });
    ParticipantId participant{};
    if (reusable != reconnect_slots_.end()) {
        participant = reusable->participant;
        if (auto restored = session_->set_participant_state(participant, model::ParticipantState::Connected); restored) {
            reconnect_slots_.erase(reusable);
            record_event("« " + connection.guest_name + " » a repris sa place");
        } else {
            participant = ParticipantId{};
        }
    }
    if (participant.value == 0) {
        auto admitted = session_->admit_verified_guest(Role::Observer, ContextId{next_context_++});
        if (!admitted) {
            counters_.denied++;
            record_event("admission refusée pour « " + connection.guest_name + " » : " + std::string(to_string(admitted.error().code)));
            disconnect(connection, admitted.error().code, admitted.error().message, true);
            return;
        }
        participant = admitted.value();
    }
    connection.participant = participant;
    connection.phase = Connection::Phase::Authed;
    connection.last_activity_ms = now;
    connection.next_heartbeat_ms = now + config_.heartbeat_interval_ms;
    connection.request_id = 0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
        [&](const PendingJoin& join) { return join.guest_name == connection.guest_name && join.address == connection.address.text; }), pending_.end());
    auto& meta = participant_meta_[participant];
    meta.id = participant;
    meta.name = connection.guest_name;
    meta.address = connection.address.display(connection.port);
    meta.client = connection.client;
    meta.connected_ms = clock_.now_wall_ms();
    meta.last_seen_ms = now;
    counters_.admitted++;
    record_event("« " + connection.guest_name + " » a rejoint la session (participant " + std::to_string(participant.value) + ")");
    HostAuth accepted;
    accepted.accepted = true;
    accepted.reason = ErrorCode::Ok;
    accepted.participant = participant;
    accepted.host_name = config_.host_name;
    accepted.session_name = config_.session_name;
    accepted.approval_required = config_.approval_required;
    queue_frame(connection, model::MessageType::Auth, encode(accepted));
    queue_frame(connection, model::MessageType::SessionState, [&] {
        auto info = build_session_info();
        for (auto& peer : info.peers) peer.self = peer.id == participant;
        return encode(info);
    }());
    queue_frame(connection, model::MessageType::ServiceList, encode(build_service_list()));
    ChatText notice;
    notice.sender = host_participant_;
    notice.sender_name = config_.host_name;
    notice.body = connection.guest_name + " a rejoint la session";
    notice.sent_ms = clock_.now_wall_ms();
    notice.system = true;
    broadcast(model::MessageType::Chat, encode(notice), ParticipantId{});
    broadcast_session_state();
    publish();
}

void HostServer::approve_request(std::uint64_t request_id) {
    const auto entry = std::find_if(pending_.begin(), pending_.end(),
        [&](const PendingJoin& join) { return join.request_id == request_id; });
    if (entry == pending_.end()) {
        if (warnings_.size() < kMaxWarnings) warnings_.push_back({ErrorCode::NotFound, "demande de connexion introuvable", "host"});
        ++revision_;
        return;
    }
    const auto name = entry->guest_name;
    const auto address = entry->address;
    pending_.erase(entry);
    for (auto& connection : connections_) {
        if (connection->phase == Connection::Phase::AwaitingApproval && connection->request_id == request_id) {
            admit(*connection);
            return;
        }
    }
    counters_.denied++;
    record_event("demande de « " + name + " » (" + address + ") expirée avant décision");
    ++revision_;
}

void HostServer::deny_request(std::uint64_t request_id, const std::string& reason) {
    const auto entry = std::find_if(pending_.begin(), pending_.end(),
        [&](const PendingJoin& join) { return join.request_id == request_id; });
    if (entry == pending_.end()) {
        if (warnings_.size() < kMaxWarnings) warnings_.push_back({ErrorCode::NotFound, "demande de connexion introuvable", "host"});
        ++revision_;
        return;
    }
    const auto name = entry->guest_name;
    pending_.erase(entry);
    counters_.denied++;
    for (auto& connection : connections_) {
        if (connection->request_id == request_id && connection->phase == Connection::Phase::AwaitingApproval) {
            disconnect(*connection, ErrorCode::PermissionDenied, reason.empty() ? std::string("demande refusée par l'hôte") : reason, true);
            break;
        }
    }
    record_event("demande de « " + name + " » refusée par l'hôte");
    publish();
}

void HostServer::revoke_participant(ParticipantId participant) {
    if (participant.value == 0 || participant == host_participant_) return;
    const auto* existing = session_->find_participant(participant);
    if (!existing) {
        if (warnings_.size() < kMaxWarnings) warnings_.push_back({ErrorCode::NotFound, "participant inconnu", "host"});
        ++revision_;
        return;
    }
    if (auto revoked = session_->revoke_participant(participant); !revoked) {
        if (warnings_.size() < kMaxWarnings) warnings_.push_back(revoked.error());
        ++revision_;
        return;
    }
    counters_.revoked++;
    const auto meta = participant_meta_.find(participant);
    const auto name = meta == participant_meta_.end() ? std::to_string(participant.value) : meta->second.name;
    reconnect_slots_.erase(std::remove_if(reconnect_slots_.begin(), reconnect_slots_.end(),
        [&](const ReconnectSlot& slot) { return slot.participant == participant; }), reconnect_slots_.end());
    for (auto& connection : connections_) {
        if (connection->participant == participant) disconnect(*connection, ErrorCode::PermissionDenied, "accès révoqué par l'hôte", true);
    }
    record_event("accès de « " + name + " » révoqué");
    ChatText notice;
    notice.sender = host_participant_;
    notice.sender_name = config_.host_name;
    notice.body = name + " a été révoqué";
    notice.sent_ms = clock_.now_wall_ms();
    notice.system = true;
    broadcast(model::MessageType::Chat, encode(notice), ParticipantId{});
    broadcast_session_state();
    publish();
}

void HostServer::handle_chat(Connection& connection, const model::Envelope& envelope) {
    auto chat = decode_chat(envelope.payload);
    if (!chat) {
        counters_.invalid_frames++;
        disconnect(connection, chat.error().code, "message de messagerie invalide", true);
        return;
    }
    // L'identité d'expéditeur vient de la connexion authentifiée, jamais du payload.
    const auto body = sanitize_chat_body(chat.value().body);
    if (!body) {
        queue_frame(connection, model::MessageType::Error, encode(ErrorNotice{ErrorCode::InvalidArgument, body.error().message}));
        if (warnings_.size() < kMaxWarnings) warnings_.push_back(body.error());
        ++revision_;
        return;
    }
    const auto recipient = chat.value().recipient;
    if (recipient.value != 0) {
        if (!session_->find_participant(recipient) || recipient == connection.participant) {
            queue_frame(connection, model::MessageType::Error, encode(ErrorNotice{ErrorCode::PermissionDenied, "destinataire privé invalide"}));
            ++revision_;
            return;
        }
        auto posted = messaging_->post_dm(connection.participant, recipient, body.value());
        if (!posted) {
            queue_frame(connection, model::MessageType::Error, encode(ErrorNotice{posted.error().code, posted.error().message}));
            if (warnings_.size() < kMaxWarnings) warnings_.push_back(posted.error());
            ++revision_;
            return;
        }
        model::ChatMessage line;
        line.id = posted.value();
        line.sender = connection.participant;
        line.recipient = recipient;
        line.body = body.value();
        line.sent_ms = clock_.now_monotonic_ms();
        auto& stored = direct_channels_[direct_key(connection.participant, recipient)];
        stored.push_back(line);
        if (stored.size() > 200) stored.pop_front();
        relay_chat(line, connection.guest_name, true, recipient);
        counters_.chat_relayed++;
        if (const auto meta = participant_meta_.find(connection.participant); meta != participant_meta_.end()) meta->second.chat_messages++;
        ++revision_;
        publish();
        return;
    }
    auto posted = messaging_->post_general(connection.participant, body.value());
    if (!posted) {
        queue_frame(connection, model::MessageType::Error, encode(ErrorNotice{posted.error().code, posted.error().message}));
        if (warnings_.size() < kMaxWarnings) warnings_.push_back(posted.error());
        ++revision_;
        return;
    }
    model::ChatMessage line;
    line.id = posted.value();
    line.sender = connection.participant;
    line.body = body.value();
    line.sent_ms = clock_.now_monotonic_ms();
    relay_chat(line, connection.guest_name, false, ParticipantId{});
    counters_.chat_relayed++;
    participant_meta_[connection.participant].chat_messages++;
    ++revision_;
    publish();
}

void HostServer::handle_heartbeat(Connection& connection, const model::Envelope& envelope) {
    auto beat = decode_heartbeat(envelope.payload);
    if (!beat) {
        counters_.invalid_frames++;
        disconnect(connection, beat.error().code, "battement invalide", true);
        return;
    }
    const auto now = clock_.now_monotonic_ms();
    connection.last_activity_ms = now;
    if (const auto meta = participant_meta_.find(connection.participant); meta != participant_meta_.end())
        meta->second.last_seen_ms = now;
    if (beat.value().echo != 0 && beat.value().echo == connection.heartbeat_echo) {
        // Réponse à notre propre mesure : l'aller-retour est réel, pas estimé.
        connection.heartbeat_echo = 0;
        connection.latency_ms = static_cast<std::uint32_t>(now - connection.heartbeat_sent_ms);
        if (const auto meta = participant_meta_.find(connection.participant); meta != participant_meta_.end())
            meta->second.latency_ms = connection.latency_ms;
    } else {
        // Demande de l'invité : on renvoie son jeton tel quel pour qu'il mesure.
        Heartbeat reply;
        reply.monotonic_ms = now;
        reply.echo = beat.value().echo;
        queue_frame(connection, model::MessageType::Heartbeat, encode(reply));
    }
    connection.next_heartbeat_ms = now + config_.heartbeat_interval_ms;
    ++revision_;
}

void HostServer::handle_bye(Connection& connection, const model::Envelope& envelope) {
    auto bye = decode_bye(envelope.payload);
    const auto reason = bye ? bye.value().text : std::string("départ de l'invité");
    disconnect(connection, ErrorCode::Cancelled, reason, false);
}

bool HostServer::notify_refusal(Connection& connection, ErrorCode code, const std::string& text) {
    if (!connection.socket.valid()) return false;
    if (connection.participant.value != 0) {
        ByeNotice bye{code, text};
        return queue_frame(connection, model::MessageType::Bye, encode(bye));
    }
    HostAuth refusal;
    refusal.accepted = false;
    refusal.awaiting_approval = false;
    refusal.reason = code;
    refusal.reason_text = text;
    refusal.host_name = config_.host_name;
    refusal.session_name = config_.session_name;
    refusal.approval_required = config_.approval_required;
    return queue_frame(connection, model::MessageType::Auth, encode(refusal));
}

void HostServer::disconnect(Connection& connection, ErrorCode code, std::string text, bool notify_peer) {
    const auto now = clock_.now_monotonic_ms();
    if (connection.phase == Connection::Phase::AwaitingApproval && connection.request_id != 0) {
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
            [&](const PendingJoin& join) { return join.request_id == connection.request_id; }), pending_.end());
    }
    if (connection.participant.value != 0 && session_) {
        const auto* participant = session_->find_participant(connection.participant);
        if (participant && participant->state != model::ParticipantState::Revoked) {
            (void)session_->set_participant_state(connection.participant, model::ParticipantState::Disconnected);
            reconnect_slots_.push_back(ReconnectSlot{connection.participant, connection.guest_name, now});
            if (reconnect_slots_.size() > 64) reconnect_slots_.pop_back();
            record_event("« " + connection.guest_name + " » est déconnecté (" + text + ")");
            ChatText notice;
            notice.sender = host_participant_;
            notice.sender_name = config_.host_name;
            notice.body = connection.guest_name + " est déconnecté";
            notice.sent_ms = clock_.now_wall_ms();
            notice.system = true;
            broadcast(model::MessageType::Chat, encode(notice), connection.participant);
            broadcast_session_state();
        }
    }
    if (notify_peer) notify_refusal(connection, code, text);
    if (code == ErrorCode::TimedOut) counters_.timeouts++;
    connection.phase = Connection::Phase::Closing;
    connection.close_deadline_ms = now + (notify_peer ? 250 : 0);
    if (!notify_peer) connection.socket.close();
    ++revision_;
    publish();
}

void HostServer::apply_timeouts() {
    const auto now = clock_.now_monotonic_ms();
    for (auto& connection : connections_) {
        switch (connection->phase) {
        case Connection::Phase::AwaitingHello:
            if (now - connection->opened_ms > config_.auth_timeout_ms) {
                counters_.timeouts++;
                disconnect(*connection, ErrorCode::TimedOut, "aucune demande d'authentification dans le délai", true);
            }
            break;
        case Connection::Phase::AwaitingApproval:
            if (now - connection->opened_ms > config_.approval_timeout_ms) {
                counters_.timeouts++;
                disconnect(*connection, ErrorCode::TimedOut, "demande en attente expirée", true);
            }
            break;
        case Connection::Phase::Authed: {
            if (now - connection->last_activity_ms > config_.inactivity_timeout_ms) {
                counters_.timeouts++;
                disconnect(*connection, ErrorCode::TimedOut, "inactivité au-delà du délai", true);
                break;
            }
            if (now >= connection->next_heartbeat_ms) {
                Heartbeat beat;
                beat.monotonic_ms = now;
                beat.echo = heartbeat_token_++; // jeton de mesure, renvoyé tel quel.
                connection->heartbeat_echo = beat.echo;
                connection->heartbeat_sent_ms = now;
                connection->next_heartbeat_ms = now + config_.heartbeat_interval_ms;
                queue_frame(*connection, model::MessageType::Heartbeat, encode(beat));
            }
            break;
        }
        case Connection::Phase::Closing:
            if (now >= connection->close_deadline_ms) connection->socket.close();
            break;
        }
    }
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
        [](const std::unique_ptr<Connection>& connection) { return !connection->socket.valid(); }), connections_.end());
    if (session_ && session_->is_expired()) {
        record_event("durée de vie de la session atteinte : fermeture");
        running_.store(false);
    }
    if (!pending_.empty()) {
        const auto wall = clock_.now_wall_ms();
        for (auto join = pending_.begin(); join != pending_.end();) {
            if (wall - join->requested_ms > config_.approval_timeout_ms) {
                record_event("demande de « " + join->guest_name + " » expirée sans décision");
                join = pending_.erase(join);
                ++revision_;
            } else ++join;
        }
    }
    if (revision_ != published_revision_) publish();
}

void HostServer::publish() {
    auto snapshot = std::make_shared<HostSnapshot>();
    snapshot->running = running_.load();
    snapshot->revision = revision_;
    snapshot->session_id = session_id_;
    snapshot->session_name = config_.session_name;
    snapshot->host_name = config_.host_name;
    snapshot->invite_code = config_.invite_code.text;
    snapshot->control_address = config_.control_address;
    snapshot->control_port = control_port_;
    snapshot->started_ms = started_ms_;
    snapshot->state = session_ ? session_->state() : model::SessionState::Closed;
    snapshot->services = services_;
    snapshot->counters = counters_;
    snapshot->connection_count = connections_.size();
    for (const auto& join : pending_) snapshot->pending.push_back(join);
    for (const auto& [id, meta] : participant_meta_) {
        const auto* participant = session_ ? session_->find_participant(id) : nullptr;
        if (!participant || participant->role == Role::Host) continue;
        ParticipantRecord record;
        record.id = id;
        record.name = meta.name;
        record.address = meta.address;
        record.state = participant->state;
        record.connected_ms = meta.connected_ms;
        record.last_seen_ms = meta.last_seen_ms;
        record.latency_ms = meta.latency_ms;
        record.chat_messages = meta.chat_messages;
        record.client = meta.client;
        snapshot->participants.push_back(std::move(record));
    }
    snapshot->chat = build_chat();
    snapshot->events.assign(events_.begin(), events_.end());
    snapshot->warnings = warnings_;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        view_ = std::move(snapshot);
        published_revision_ = revision_;
    }
    published_.notify_all();
}

} // namespace plpt::net
