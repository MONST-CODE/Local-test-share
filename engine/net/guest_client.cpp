// PLPT — engine/net/guest_client.cpp — fil de contrôle invité : connexion,
// authentification par code d'invitation, attente d'autorisation, messagerie,
// réception des services réellement publiés par l'hôte.
#include "guest_client.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include "../core/format.hpp"

namespace plpt::net {
namespace {
constexpr std::size_t kMaxOutboxBytes = 256 * 1024;
constexpr std::size_t kMaxEvents = 200;
constexpr std::size_t kMaxWarnings = 32;
constexpr std::size_t kChatCapacity = 200;
constexpr std::size_t kMaxReceivePerPoll = 16;

std::string display_host(const IpAddress& address, std::uint16_t port) {
    return address.display(Port{port});
}
} // namespace

GuestClient::~GuestClient() { stop(); }

Result<void> GuestClient::start(GuestConfig config) {
    if (running_.load()) return Result<void>::failure({ErrorCode::InvalidState, "client déjà démarré", "guest"});
    if (config.control_port == 0)
        return Result<void>::failure({ErrorCode::InvalidArgument, "port de contrôle de l'hôte manquant", "guest"});
    if (!config.address.valid())
        return Result<void>::failure({ErrorCode::InvalidArgument, "adresse d'hôte invalide", "guest"});
    auto name = sanitize_name(config.guest_name, kMaxNameBytes);
    if (!name) return Result<void>::failure(name.error());
    config.guest_name = name.value();
    if (!config.invite_code.valid())
        return Result<void>::failure({ErrorCode::InvalidArgument, "code d'invitation manquant : l'hôte refusera toute demande", "guest"});
    if (config.connect_timeout_ms == 0 || config.heartbeat_interval_ms == 0 || config.inactivity_timeout_ms == 0)
        return Result<void>::failure({ErrorCode::InvalidArgument, "délais réseau nuls", "guest"});
    if (auto signal = wakeup_.open(); !signal) return Result<void>::failure(signal.error());

    config_ = std::move(config);
    auto created = Socket::create(TransportProtocol::Tcp, config_.address.family);
    if (!created) return Result<void>::failure(created.error());
    socket_ = std::move(created).value();
    if (auto delay = socket_.set_no_delay(true); !delay) return Result<void>::failure(delay.error());

    state_ = GuestSnapshot{};
    state_.host_address = display_host(config_.address, config_.control_port);
    state_.host_port = config_.control_port;
    state_.phase = GuestPhase::Connecting;
    state_.status_text = "connexion à " + state_.host_address;
    state_.approval_required = true;
    chat_.clear();
    events_.clear();
    assembler_ = FrameAssembler{};
    outbox_.clear();
    outbox_sent_ = 0;
    revision_ = 0;
    published_revision_ = 0;
    connected_ = false;
    heartbeat_token_ = 1;
    outstanding_echo_ = 0;
    sequence_out_ = 0;
    started_ms_ = clock_.now_monotonic_ms();
    last_activity_ms_ = started_ms_;
    next_heartbeat_ms_ = started_ms_ + config_.heartbeat_interval_ms;

    record_event("tentative de connexion à " + state_.host_address);
    running_.store(true);
    publish();
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return Result<void>::success();
}

void GuestClient::stop() {
    const bool was_running = running_.load();
    running_.store(false);
    if (worker_.joinable()) {
        worker_.request_stop();
        wakeup_.notify();
        worker_.join();
    }
    if (was_running) {
        if (state_.phase == GuestPhase::Connected) set_phase(GuestPhase::Stopped, "arrêt demandé localement");
        publish();
    }
    socket_.close();
}

bool GuestClient::enqueue(Command command) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_.load() && command.kind != CommandKind::Stop) return false;
        if (commands_.size() >= 128) return false; // file bornée : jamais de croissance infinie.
        commands_.push_back(std::move(command));
    }
    wakeup_.notify();
    return true;
}

bool GuestClient::post_chat(std::string body) {
    return enqueue(Command{CommandKind::PostChat, ParticipantId{}, std::move(body)});
}

bool GuestClient::post_direct_message(ParticipantId peer, std::string body) {
    if (peer.value == 0 || peer == state_.self) return false;
    {
        // La liste des pairs publiée par l'hôte est la seule autorité dont dispose
        // l'invité : un destinataire absent est refusé avant même d'émettre.
        std::lock_guard<std::mutex> guard(mutex_);
        if (!view_) return false;
        const bool known = std::any_of(view_->peers.begin(), view_->peers.end(),
            [&](const GuestPeer& known_peer) { return known_peer.id == peer; });
        if (!known) return false;
    }
    return enqueue(Command{CommandKind::PostDirect, peer, std::move(body)});
}

bool GuestClient::leave() { return enqueue(Command{CommandKind::Leave, ParticipantId{}, ""}); }

std::shared_ptr<const GuestSnapshot> GuestClient::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return view_ ? view_ : std::make_shared<const GuestSnapshot>();
}

bool GuestClient::wait_for(const std::function<bool(const GuestSnapshot&)>& predicate, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(mutex_);
    return published_.wait_for(guard, timeout, [&] { return view_ && predicate(*view_); });
}

void GuestClient::record_event(std::string text) {
    events_.push_back(format_clock_utc(clock_.now_wall_ms()) + "  " + std::move(text));
    while (events_.size() > kMaxEvents) events_.pop_front();
    ++revision_;
}

void GuestClient::record_warning(Error error) {
    if (state_.warnings.size() < kMaxWarnings) state_.warnings.push_back(std::move(error));
    ++revision_;
}

void GuestClient::set_phase(GuestPhase phase, std::string text, ErrorCode code) {
    const bool changed = state_.phase != phase;
    state_.phase = phase;
    state_.status_text = std::move(text);
    if (code != ErrorCode::Ok) { state_.last_error = code; state_.counters.errors++; }
    if (changed) record_event(guest_phase_text(phase) + std::string(" : ") + state_.status_text);
    ++revision_;
}

void GuestClient::push_chat(ChatLine line) {
    chat_.push_back(std::move(line));
    while (chat_.size() > kChatCapacity) chat_.pop_front();
    ++revision_;
}

void GuestClient::run(std::stop_token token) {
    if (auto connected = connect_and_authenticate(); !connected) {
        set_phase(state_.phase == GuestPhase::Refused ? GuestPhase::Refused : GuestPhase::Disconnected,
            connected.error().message, connected.error().code);
        running_.store(false);
        publish();
        return;
    }
    connected_ = true;
    while (!token.stop_requested() && running_.load()) {
        {
            std::deque<Command> pending;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                pending.swap(commands_);
            }
            for (const auto& command : pending) {
                if (command.kind == CommandKind::Stop) {
                    running_.store(false);
                    break;
                }
                if (command.kind == CommandKind::Leave) {
                    ByeNotice bye{ErrorCode::Cancelled, "départ de l'invité"};
                    queue_frame(model::MessageType::Bye, encode(bye));
                    flush(std::chrono::milliseconds(300));
                    set_phase(GuestPhase::Stopped, "départ envoyé à l'hôte");
                    running_.store(false);
                    break;
                }
                auto body = sanitize_chat_body(command.text);
                if (!body) { record_warning(body.error()); continue; }
                ChatText message;
                message.sender = state_.self;
                message.recipient = command.kind == CommandKind::PostDirect ? command.recipient : ParticipantId{};
                message.sender_name = config_.guest_name;
                message.body = body.value();
                message.sent_ms = clock_.now_wall_ms();
                if (!queue_frame(model::MessageType::Chat, encode(message))) continue;
                state_.counters.chat_sent++;
                // L'invité n'affiche pas sa propre bulle avant l'écho de l'hôte :
                // ce qu'il voit est toujours ce que la session a réellement accepté.
                ++revision_;
            }
        }
        if (!running_.load()) break;
        const auto now = clock_.now_monotonic_ms();
        if (state_.phase == GuestPhase::Connected && now >= next_heartbeat_ms_) {
            Heartbeat beat;
            beat.monotonic_ms = now;
            beat.echo = heartbeat_token_++; // jeton de mesure, renvoyé tel quel par l'hôte.
            outstanding_echo_ = beat.echo;
            heartbeat_sent_ms_ = now;
            queue_frame(model::MessageType::Heartbeat, encode(beat));
            next_heartbeat_ms_ = now + config_.heartbeat_interval_ms;
        }
        if (now - last_activity_ms_ > config_.inactivity_timeout_ms) {
            set_phase(GuestPhase::Disconnected, "aucune réponse de l'hôte au-delà du délai", ErrorCode::TimedOut);
            state_.counters.errors++;
            break;
        }
        poll_once(std::chrono::milliseconds(100));
        while (outbox_sent_ < outbox_.size()) {
            auto sent = socket_.send({outbox_.data() + outbox_sent_, outbox_.size() - outbox_sent_});
            if (!sent) break; // WouldBlock : le reste part au prochain tour.
            if (sent.value() == 0) break;
            outbox_sent_ += sent.value();
        }
        if (outbox_sent_ == outbox_.size()) { outbox_.clear(); outbox_sent_ = 0; }
        else if (outbox_sent_ > 0) {
            outbox_.erase(outbox_.begin(), outbox_.begin() + static_cast<std::ptrdiff_t>(outbox_sent_));
            outbox_sent_ = 0;
        }
        if (revision_ != published_revision_) publish();
    }
    if (running_.load()) {
        ByeNotice bye{ErrorCode::Cancelled, "arrêt local"};
        queue_frame(model::MessageType::Bye, encode(bye));
        flush(std::chrono::milliseconds(200));
        running_.store(false);
    }
    socket_.close();
    publish();
}

Result<void> GuestClient::connect_and_authenticate() {
    if (auto connected = connect_with_timeout(socket_, config_.address, Port{config_.control_port},
            std::chrono::milliseconds(config_.connect_timeout_ms)); !connected) {
        set_phase(GuestPhase::Disconnected, "connexion impossible à " + state_.host_address + " : " + connected.error().message,
            connected.error().code);
        return Result<void>::failure(connected.error());
    }
    (void)socket_.set_nonblocking(true);
    GuestHello hello;
    hello.guest_name = config_.guest_name;
    hello.invite_code = config_.invite_code.text; // seule occurrence en clair : sur le fil, jamais dans les journaux.
    hello.client = config_.client;
    const auto payload = encode(hello);
    // Hello : session et participant inconnus, séquence nulle — c'est la seule
    // trame du protocole qui le permet.
    auto framed = frame(model::MessageType::Hello, SessionId{}, ParticipantId{}, 0, payload);
    if (!framed) return Result<void>::failure(framed.error());
    if (auto written = write_all(socket_, framed.value(), std::chrono::milliseconds(config_.connect_timeout_ms)); !written) {
        set_phase(GuestPhase::Disconnected, "demande d'authentification non transmise", written.error().code);
        return Result<void>::failure(written.error());
    }
    set_phase(GuestPhase::Authenticating, "code d'invitation transmis, vérification par l'hôte");
    const auto deadline = clock_.now_monotonic_ms() + config_.approval_timeout_ms;
    std::array<std::uint8_t, 8192> buffer{};
    while (clock_.now_monotonic_ms() < deadline) {
        {
            std::deque<Command> pending;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                pending.swap(commands_);
            }
            for (const auto& command : pending) {
                if (command.kind == CommandKind::Stop) { running_.store(false); return Result<void>::failure({ErrorCode::Cancelled, "arrêt demandé", "guest"}); }
                if (command.kind == CommandKind::Leave) { running_.store(false); return Result<void>::failure({ErrorCode::Cancelled, "départ avant admission", "guest"}); }
            }
        }
        std::vector<PollEntry> entries{PollEntry{socket_.native(), true, false, false, false, false},
            PollEntry{wakeup_.native(), true, false, false, false, false}};
        auto waited = poll_sockets(entries, std::chrono::milliseconds(100));
        if (!waited) return Result<void>::failure(waited.error());
        if (entries[1].readable) { wakeup_.drain(); continue; }
        if (!entries[0].readable && !entries[0].failed) continue;
        if (entries[0].failed) {
            set_phase(GuestPhase::Disconnected, "connexion interrompue pendant l'authentification", ErrorCode::Disconnected);
            return Result<void>::failure({ErrorCode::Disconnected, "connexion interrompue", "guest"});
        }
        auto received = socket_.receive(buffer);
        if (!received) {
            if (received.error().code == ErrorCode::WouldBlock) continue;
            set_phase(GuestPhase::Disconnected, received.error().message, received.error().code);
            return Result<void>::failure(received.error());
        }
        if (received.value() == 0) {
            set_phase(GuestPhase::Disconnected, "l'hôte a fermé la connexion pendant l'authentification", ErrorCode::Disconnected);
            return Result<void>::failure({ErrorCode::Disconnected, "connexion fermée par l'hôte", "guest"});
        }
        last_activity_ms_ = clock_.now_monotonic_ms();
        if (auto fed = assembler_.feed({buffer.data(), received.value()}); !fed) {
            set_phase(GuestPhase::Refused, "flux de contrôle invalide : " + fed.error().message, fed.error().code);
            return Result<void>::failure(fed.error());
        }
        bool progress = true;
        while (progress) {
            auto frame_data = assembler_.next();
            if (!frame_data) {
                progress = false;
                if (frame_data.error().code != ErrorCode::WouldBlock) {
                    state_.counters.invalid_frames++;
                    set_phase(GuestPhase::Refused, frame_data.error().message, frame_data.error().code);
                    return Result<void>::failure(frame_data.error());
                }
                break;
            }
            state_.counters.frames_received++;
            const auto& envelope = frame_data.value();
            if (envelope.type == model::MessageType::Auth) {
                auto auth = decode_host_auth(envelope.payload);
                if (!auth) { set_phase(GuestPhase::Refused, auth.error().message, auth.error().code); return Result<void>::failure(auth.error()); }
                const auto& reply = auth.value();
                state_.host_name = reply.host_name;
                state_.session_name = reply.session_name;
                state_.approval_required = reply.approval_required;
                state_.session_id = envelope.session.value;
                if (reply.awaiting_approval && !reply.accepted) {
                    set_phase(GuestPhase::AwaitingApproval, reply.reason_text.empty() ? std::string("code valide : en attente d'autorisation de l'hôte") : reply.reason_text);
                    publish();
                    continue;
                }
                if (!reply.accepted) {
                    set_phase(GuestPhase::Refused, reply.reason_text.empty() ? std::string("demande refusée par l'hôte") : reply.reason_text, reply.reason);
                    return Result<void>::failure({reply.reason == ErrorCode::Ok ? ErrorCode::Unauthorized : reply.reason, reply.reason_text, "guest"});
                }
                state_.self = reply.participant;
                state_.connected_ms = clock_.now_wall_ms();
                set_phase(GuestPhase::Connected, "admis dans « " + state_.session_name + " »");
                // L'hôte envoie SessionState et ServiceList juste après Auth : les
                // traiter ici évite à l'interface d'afficher « 0 service » alors que
                // la liste est déjà arrivée sur le même segment TCP.
                drain_buffered_frames();
                publish();
                return Result<void>::success();
            }
            if (envelope.type == model::MessageType::Bye) {
                auto bye = decode_bye(envelope.payload);
                const auto text = bye ? bye.value().text : std::string("connexion refusée par l'hôte");
                const auto code = bye ? bye.value().reason : ErrorCode::Disconnected;
                set_phase(code == ErrorCode::Unauthorized ? GuestPhase::Refused : GuestPhase::Disconnected, text, code);
                return Result<void>::failure({code, text, "guest"});
            }
            if (envelope.type == model::MessageType::Error) {
                auto notice = decode_error(envelope.payload);
                const auto text = notice ? notice.value().text : std::string("erreur de l'hôte");
                const auto code = notice ? notice.value().reason : ErrorCode::ProtocolViolation;
                set_phase(GuestPhase::Refused, text, code);
                return Result<void>::failure({code, text, "guest"});
            }
            // SessionState ou ServiceList avant admission : l'hôte ne doit pas en
            // envoyer, mais les accepter évite de couper une session fonctionnelle.
            handle_frame(envelope);
        }
    }
    set_phase(GuestPhase::Disconnected, "aucune réponse de l'hôte dans le délai d'authentification", ErrorCode::TimedOut);
    return Result<void>::failure({ErrorCode::TimedOut, "authentification expirée", "guest"});
}

void GuestClient::drain_buffered_frames() {
    while (running_.load()) {
        auto frame_data = assembler_.next();
        if (!frame_data) {
            if (frame_data.error().code != ErrorCode::WouldBlock) {
                state_.counters.invalid_frames++;
                record_warning(frame_data.error());
            }
            return;
        }
        state_.counters.frames_received++;
        handle_frame(frame_data.value());
    }
}

void GuestClient::poll_once(std::chrono::milliseconds timeout) {
    std::vector<PollEntry> entries{PollEntry{socket_.native(), true, false, false, false, false},
        PollEntry{wakeup_.native(), true, false, false, false, false}};
    auto waited = poll_sockets(entries, timeout);
    if (!waited) { record_warning(waited.error()); return; }
    if (entries[1].readable) { wakeup_.drain(); return; }
    if (entries[0].failed) {
        set_phase(GuestPhase::Disconnected, "connexion interrompue", ErrorCode::Disconnected);
        running_.store(false);
        return;
    }
    if (!entries[0].readable) return;
    std::array<std::uint8_t, 8192> buffer{};
    for (std::size_t round = 0; round < kMaxReceivePerPoll; ++round) {
        auto received = socket_.receive(buffer);
        if (!received) {
            if (received.error().code != ErrorCode::WouldBlock) {
                set_phase(GuestPhase::Disconnected, received.error().message, received.error().code);
                running_.store(false);
            }
            return;
        }
        if (received.value() == 0) {
            set_phase(GuestPhase::Disconnected, "l'hôte a fermé la connexion", ErrorCode::Disconnected);
            running_.store(false);
            return;
        }
        last_activity_ms_ = clock_.now_monotonic_ms();
        if (auto fed = assembler_.feed({buffer.data(), received.value()}); !fed) {
            set_phase(GuestPhase::Disconnected, fed.error().message, fed.error().code);
            running_.store(false);
            return;
        }
        bool progress = true;
        while (progress) {
            auto frame_data = assembler_.next();
            if (!frame_data) {
                progress = false;
                if (frame_data.error().code != ErrorCode::WouldBlock) {
                    state_.counters.invalid_frames++;
                    set_phase(GuestPhase::Disconnected, frame_data.error().message, frame_data.error().code);
                    running_.store(false);
                }
                break;
            }
            state_.counters.frames_received++;
            handle_frame(frame_data.value());
        }
    }
}

void GuestClient::handle_frame(const model::Envelope& envelope) {
    if (envelope.session.value != 0 && state_.session_id != 0 && envelope.session.value != state_.session_id) {
        state_.counters.invalid_frames++;
        set_phase(GuestPhase::Disconnected, "identifiant de session incohérent", ErrorCode::ProtocolViolation);
        running_.store(false);
        return;
    }
    switch (envelope.type) {
    case model::MessageType::SessionState: {
        auto info = decode_session_info(envelope.payload);
        if (!info) { record_warning(info.error()); return; }
        state_.session_id = info.value().session.value;
        state_.session_name = info.value().session_name;
        state_.host_name = info.value().host_name;
        state_.peers.clear();
        for (const auto& peer : info.value().peers) {
            GuestPeer record;
            record.id = peer.id;
            record.name = peer.name;
            record.state = static_cast<model::ParticipantState>(peer.state);
            record.self = peer.self;
            record.connected_ms = peer.connected_ms;
            record.latency_ms = peer.latency_ms;
            if (record.self && state_.self.value == 0) state_.self = record.id;
            state_.peers.push_back(std::move(record));
        }
        ++revision_;
        return;
    }
    case model::MessageType::ServiceList: {
        auto list = decode_service_list(envelope.payload);
        if (!list) { record_warning(list.error()); return; }
        const auto now = clock_.now_wall_ms();
        state_.services.clear();
        for (const auto& service : list.value().services) {
            GuestService record;
            record.key = service.key;
            record.label = service.label;
            record.exposed_port = service.exposed_port;
            record.url = service.url;
            record.available = service.available;
            record.received_ms = now;
            state_.services.push_back(std::move(record));
        }
        record_event("services publiés par l'hôte : " + std::to_string(state_.services.size()));
        return;
    }
    case model::MessageType::Chat: {
        auto chat = decode_chat(envelope.payload);
        if (!chat) { record_warning(chat.error()); return; }
        ChatLine line;
        line.id = envelope.sequence;
        line.sender = chat.value().sender;
        line.sender_name = chat.value().sender_name;
        line.body = chat.value().body;
        line.sent_ms = chat.value().sent_ms;
        line.system = chat.value().system;
        line.direct = chat.value().recipient.value != 0;
        push_chat(std::move(line));
        state_.counters.chat_received++;
        if (chat.value().system) record_event(chat.value().body);
        return;
    }
    case model::MessageType::Heartbeat: {
        auto beat = decode_heartbeat(envelope.payload);
        if (!beat) { record_warning(beat.error()); return; }
        const auto now = clock_.now_monotonic_ms();
        if (beat.value().echo != 0 && beat.value().echo == outstanding_echo_) {
            // Réponse à notre mesure : latence aller-retour réelle.
            outstanding_echo_ = 0;
            state_.latency_ms = static_cast<std::uint32_t>(now - heartbeat_sent_ms_);
        } else if (state_.phase == GuestPhase::Connected) {
            // Demande de l'hôte : son jeton est renvoyé tel quel, rien d'autre.
            Heartbeat reply;
            reply.monotonic_ms = now;
            reply.echo = beat.value().echo;
            queue_frame(model::MessageType::Heartbeat, encode(reply));
        }
        ++revision_;
        return;
    }
    case model::MessageType::Error: {
        auto notice = decode_error(envelope.payload);
        if (!notice) { record_warning(notice.error()); return; }
        state_.counters.errors++;
        record_event("refus de l'hôte : " + notice.value().text);
        return;
    }
    case model::MessageType::Bye: {
        auto bye = decode_bye(envelope.payload);
        const auto text = bye ? bye.value().text : std::string("session terminée par l'hôte");
        set_phase(GuestPhase::Disconnected, text, bye ? bye.value().reason : ErrorCode::Disconnected);
        running_.store(false);
        return;
    }
    default:
        state_.counters.invalid_frames++;
        record_warning({ErrorCode::ProtocolViolation, "type de message inattendu sur le fil de contrôle", "guest"});
        return;
    }
}

bool GuestClient::queue_frame(model::MessageType type, std::span<const std::uint8_t> payload) {
    if (outbox_.size() - outbox_sent_ + payload.size() > kMaxOutboxBytes) {
        record_warning({ErrorCode::RateLimited, "file d'émission saturée : message abandonné", "guest"});
        return false;
    }
    // Après admission, le protocole exige un participant et une séquence non nuls.
    auto framed = frame(type, SessionId{state_.session_id}, state_.self, ++sequence_out_, payload);
    if (!framed) { record_warning(framed.error()); return false; }
    outbox_.insert(outbox_.end(), framed.value().begin(), framed.value().end());
    ++revision_;
    return true;
}

void GuestClient::flush(std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (outbox_sent_ < outbox_.size() && std::chrono::steady_clock::now() < deadline) {
        std::vector<PollEntry> entries{PollEntry{socket_.native(), false, true, false, false, false}};
        auto waited = poll_sockets(entries, std::chrono::milliseconds(50));
        if (!waited) return;
        if (!entries[0].writable) continue;
        auto sent = socket_.send({outbox_.data() + outbox_sent_, outbox_.size() - outbox_sent_});
        if (!sent) return;
        outbox_sent_ += sent.value();
    }
}

void GuestClient::publish() {
    auto snapshot = std::make_shared<GuestSnapshot>(state_);
    snapshot->revision = revision_;
    snapshot->chat.assign(chat_.begin(), chat_.end());
    snapshot->events.assign(events_.begin(), events_.end());
    {
        std::lock_guard<std::mutex> guard(mutex_);
        view_ = std::move(snapshot);
        published_revision_ = revision_;
    }
    published_.notify_all();
}

} // namespace plpt::net
