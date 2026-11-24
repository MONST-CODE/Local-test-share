// PLPT — engine/net/wire.cpp — sérialisation stricte des messages LAN.
#include "wire.hpp"

#include <algorithm>
#include <cstring>
#include "../core/digest.hpp"
#include "../core/mac.hpp"
#include "../core/text.hpp"

namespace plpt::net {
namespace {
constexpr std::uint32_t kFrameMagic = 0x504C5054U; // « PLPT »
constexpr std::size_t kHeaderBytes = model::kEnvelopeHeaderBytes;
constexpr std::size_t kAssemblerCapBytes = 256 * 1024;

std::uint64_t read_be(std::span<const std::uint8_t> data, std::size_t offset, unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < bytes; ++index) value = (value << 8) | data[offset + index];
    return value;
}

bool control_free(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7F;
    });
}
} // namespace

void PayloadWriter::u8(std::uint8_t value) { buffer_.push_back(value); }
void PayloadWriter::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value >> 8));
    buffer_.push_back(static_cast<std::uint8_t>(value));
}
void PayloadWriter::u32(std::uint32_t value) {
    for (unsigned shift = 24; ; shift -= 8) {
        buffer_.push_back(static_cast<std::uint8_t>(value >> shift));
        if (shift == 0) break;
    }
}
void PayloadWriter::u64(std::uint64_t value) {
    for (unsigned shift = 56; ; shift -= 8) {
        buffer_.push_back(static_cast<std::uint8_t>(value >> shift));
        if (shift == 0) break;
    }
}
void PayloadWriter::boolean(bool value) { u8(value ? 1U : 0U); }
void PayloadWriter::text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}
void PayloadWriter::blob(std::span<const std::uint8_t> value) {
    u32(static_cast<std::uint32_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}

Result<std::uint8_t> PayloadReader::u8() {
    if (remaining() < 1) return Result<std::uint8_t>::failure(truncated());
    return Result<std::uint8_t>::success(data_[offset_++]);
}
Result<std::uint16_t> PayloadReader::u16() {
    if (remaining() < 2) return Result<std::uint16_t>::failure(truncated());
    const auto value = static_cast<std::uint16_t>(read_be(data_, offset_, 2));
    offset_ += 2;
    return Result<std::uint16_t>::success(value);
}
Result<std::uint32_t> PayloadReader::u32() {
    if (remaining() < 4) return Result<std::uint32_t>::failure(truncated());
    const auto value = static_cast<std::uint32_t>(read_be(data_, offset_, 4));
    offset_ += 4;
    return Result<std::uint32_t>::success(value);
}
Result<std::uint64_t> PayloadReader::u64() {
    if (remaining() < 8) return Result<std::uint64_t>::failure(truncated());
    const auto value = read_be(data_, offset_, 8);
    offset_ += 8;
    return Result<std::uint64_t>::success(value);
}
Result<bool> PayloadReader::boolean() {
    auto value = u8();
    if (!value) return Result<bool>::failure(value.error());
    if (value.value() > 1) return Result<bool>::failure({ErrorCode::ProtocolViolation, "booléen hors 0/1", "wire"});
    return Result<bool>::success(value.value() == 1);
}
Result<std::string> PayloadReader::text(std::size_t max_bytes) {
    auto length = u32();
    if (!length) return Result<std::string>::failure(length.error());
    if (length.value() > max_bytes)
        return Result<std::string>::failure({ErrorCode::TooLarge, "texte au-delà de la borne du protocole", "wire"});
    if (remaining() < length.value()) return Result<std::string>::failure(truncated());
    std::string out(reinterpret_cast<const char*>(data_.data() + offset_), length.value());
    offset_ += length.value();
    return Result<std::string>::success(std::move(out));
}
Result<std::vector<std::uint8_t>> PayloadReader::blob(std::size_t max_bytes) {
    auto length = u32();
    if (!length) return Result<std::vector<std::uint8_t>>::failure(length.error());
    if (length.value() > max_bytes)
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::TooLarge, "bloc au-delà de la borne", "wire"});
    if (remaining() < length.value()) return Result<std::vector<std::uint8_t>>::failure(truncated());
    std::vector<std::uint8_t> out(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
        data_.begin() + static_cast<std::ptrdiff_t>(offset_ + length.value()));
    offset_ += length.value();
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}
Result<void> PayloadReader::require_end() const {
    if (!at_end()) return Result<void>::failure({ErrorCode::ProtocolViolation, "octets surnuméraires", "wire"});
    return Result<void>::success();
}

Result<std::string> sanitize_name(std::string_view raw, std::size_t max_bytes) {
    auto trimmed = raw;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.remove_prefix(1);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\n')) trimmed.remove_suffix(1);
    if (trimmed.empty()) return Result<std::string>::failure({ErrorCode::InvalidArgument, "nom vide", "wire"});
    if (trimmed.size() > max_bytes)
        return Result<std::string>::failure({ErrorCode::TooLarge, "nom trop long", "wire"});
    if (!valid_text(trimmed) || !control_free(trimmed))
        return Result<std::string>::failure({ErrorCode::InvalidArgument, "nom non UTF-8 ou contenant des contrôles", "wire"});
    return Result<std::string>::success(std::string(trimmed));
}

Result<std::string> sanitize_chat_body(std::string_view raw) {
    // Les deux extrémités sont rognées : un message collé depuis un navigateur
    // arrive souvent entouré de sauts de ligne, et les refuser en bloc serait un
    // faux échec. Le contenu interne, lui, est conservé tel quel.
    const auto edge = [](char character) { return character == '\n' || character == '\r' || character == ' ' || character == '\t'; };
    auto trimmed = raw;
    while (!trimmed.empty() && edge(trimmed.front())) trimmed.remove_prefix(1);
    while (!trimmed.empty() && edge(trimmed.back())) trimmed.remove_suffix(1);
    if (trimmed.empty()) return Result<std::string>::failure({ErrorCode::InvalidArgument, "message vide", "wire"});
    if (trimmed.size() > kMaxChatBytes)
        return Result<std::string>::failure({ErrorCode::TooLarge, "message au-delà de 4096 octets", "wire"});
    // CR et CRLF deviennent LF : un message multiligne reste acceptable alors
    // qu'un retour chariot brut est un caractère de contrôle interdit sur le fil.
    std::string normalized;
    normalized.reserve(trimmed.size());
    for (std::size_t index = 0; index < trimmed.size(); ++index) {
        if (trimmed[index] != '\r') { normalized.push_back(trimmed[index]); continue; }
        normalized.push_back('\n');
        if (index + 1 < trimmed.size() && trimmed[index + 1] == '\n') ++index;
    }
    if (normalized.size() > kMaxChatBytes)
        return Result<std::string>::failure({ErrorCode::TooLarge, "message au-delà de 4096 octets", "wire"});
    if (!valid_text(normalized))
        return Result<std::string>::failure({ErrorCode::InvalidArgument, "message non UTF-8 canonique", "wire"});
    if (normalized.find('\0') != std::string::npos)
        return Result<std::string>::failure({ErrorCode::InvalidArgument, "octet nul dans le message", "wire"});
    return Result<std::string>::success(std::move(normalized));
}

std::vector<std::uint8_t> encode(const GuestHello& message) {
    PayloadWriter writer;
    writer.text(message.guest_name);
    writer.text(message.invite_code);
    writer.text(message.client);
    writer.text(message.resume_secret);
    return writer.take();
}
std::vector<std::uint8_t> encode(const HostAuth& message) {
    PayloadWriter writer;
    writer.boolean(message.accepted);
    writer.u16(static_cast<std::uint16_t>(message.reason));
    writer.text(message.reason_text);
    writer.u64(message.participant.value);
    writer.text(message.host_name);
    writer.text(message.session_name);
    writer.boolean(message.approval_required);
    writer.boolean(message.awaiting_approval);
    writer.text(message.resume_secret);
    writer.u32(message.resume_ttl_ms);
    return writer.take();
}
std::vector<std::uint8_t> encode(const SessionInfo& message) {
    PayloadWriter writer;
    writer.u64(message.session.value);
    writer.text(message.session_name);
    writer.text(message.host_name);
    writer.u8(message.state);
    writer.u32(message.max_participants);
    writer.u64(message.started_ms);
    writer.u32(static_cast<std::uint32_t>(message.peers.size()));
    for (const auto& peer : message.peers) {
        writer.u64(peer.id.value);
        writer.text(peer.name);
        writer.u8(peer.state);
        writer.u64(peer.connected_ms);
        writer.u32(peer.latency_ms);
        writer.boolean(peer.self);
    }
    // Bloc 1.3 optionnel : transport réel, session puis chaque pair dans l'ordre.
    const bool extended = !message.transport.empty();
    writer.boolean(extended);
    if (extended) {
        writer.text(message.transport);
        for (const auto& peer : message.peers) writer.text(peer.transport);
    }
    // Bloc 1.5 optionnel, toujours APRÈS le bloc 1.3 : identifiant de l'hôte.
    if (message.host_participant.value != 0) writer.u64(message.host_participant.value);
    return writer.take();
}
std::vector<std::uint8_t> encode(const ServiceList& message) {
    PayloadWriter writer;
    writer.text(message.control_address);
    writer.u32(static_cast<std::uint32_t>(message.services.size()));
    for (const auto& service : message.services) {
        writer.u64(service.key);
        writer.text(service.label);
        writer.u16(service.exposed_port);
        writer.text(service.url);
        writer.boolean(service.available);
    }
    return writer.take();
}
std::vector<std::uint8_t> encode(const ChatText& message) {
    PayloadWriter writer;
    writer.u64(message.sender.value);
    writer.u64(message.recipient.value);
    writer.text(message.sender_name);
    writer.u64(message.sent_ms);
    writer.boolean(message.system);
    writer.text(message.body);
    return writer.take();
}
std::vector<std::uint8_t> encode(const FeedbackReport& message) {
    PayloadWriter writer;
    writer.u8(message.severity);
    writer.u64(message.sent_ms);
    writer.text(message.description);
    writer.text(message.context);
    return writer.take();
}
Result<FeedbackReport> decode_feedback(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    auto severity = reader.u8();
    auto sent = reader.u64();
    auto description = reader.text(kMaxFeedbackBytes);
    auto context = reader.text(kMaxFeedbackContextBytes);
    if (!severity || !sent || !description || !context)
        return Result<FeedbackReport>::failure({ErrorCode::ProtocolViolation, "signalement tronqué", "wire"});
    if (auto end = reader.require_end(); !end) return Result<FeedbackReport>::failure(end.error());
    if (severity.value() > 3)
        return Result<FeedbackReport>::failure({ErrorCode::InvalidArgument, "gravité inconnue", "wire"});
    if (!valid_text(context.value()) || !control_free(context.value()))
        return Result<FeedbackReport>::failure({ErrorCode::InvalidArgument, "contexte de signalement invalide", "wire"});
    FeedbackReport report;
    report.severity = severity.value();
    report.sent_ms = sent.value();
    report.description = std::move(description).value();
    report.context = std::move(context).value();
    return Result<FeedbackReport>::success(std::move(report));
}
std::vector<std::uint8_t> encode(const ErrorNotice& message) {
    PayloadWriter writer;
    writer.u16(static_cast<std::uint16_t>(message.reason));
    writer.text(message.text);
    return writer.take();
}
std::vector<std::uint8_t> encode(const ByeNotice& message) {
    PayloadWriter writer;
    writer.u16(static_cast<std::uint16_t>(message.reason));
    writer.text(message.text);
    return writer.take();
}
std::vector<std::uint8_t> encode(const Heartbeat& message) {
    PayloadWriter writer;
    writer.u64(message.monotonic_ms);
    writer.u64(message.echo);
    return writer.take();
}

Result<GuestHello> decode_guest_hello(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    GuestHello message;
    auto name = reader.text(kMaxNameBytes);
    if (!name) return Result<GuestHello>::failure(name.error());
    auto code = reader.text(32);
    if (!code) return Result<GuestHello>::failure(code.error());
    auto client = reader.text(kMaxNameBytes);
    if (!client) return Result<GuestHello>::failure(client.error());
    auto secret = reader.text(kMaxSecretBytes);
    if (!secret) return Result<GuestHello>::failure(secret.error());
    if (auto end = reader.require_end(); !end) return Result<GuestHello>::failure(end.error());
    message.guest_name = std::move(name).value();
    message.invite_code = std::move(code).value();
    message.client = std::move(client).value();
    message.resume_secret = std::move(secret).value();
    if (!control_free(message.guest_name) || !valid_text(message.guest_name))
        return Result<GuestHello>::failure({ErrorCode::InvalidArgument, "nom d'invité invalide", "wire"});
    if (!control_free(message.invite_code))
        return Result<GuestHello>::failure({ErrorCode::InvalidArgument, "code d'invitation invalide", "wire"});
    if (!control_free(message.resume_secret))
        return Result<GuestHello>::failure({ErrorCode::InvalidArgument, "secret de reprise invalide", "wire"});
    return Result<GuestHello>::success(std::move(message));
}

Result<HostAuth> decode_host_auth(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    HostAuth message;
    auto accepted = reader.boolean();
    if (!accepted) return Result<HostAuth>::failure(accepted.error());
    auto reason = reader.u16();
    if (!reason) return Result<HostAuth>::failure(reason.error());
    auto text = reader.text(kMaxReasonBytes);
    if (!text) return Result<HostAuth>::failure(text.error());
    auto participant = reader.u64();
    if (!participant) return Result<HostAuth>::failure(participant.error());
    auto host_name = reader.text(kMaxNameBytes);
    if (!host_name) return Result<HostAuth>::failure(host_name.error());
    auto session_name = reader.text(kMaxNameBytes);
    if (!session_name) return Result<HostAuth>::failure(session_name.error());
    auto approval = reader.boolean();
    if (!approval) return Result<HostAuth>::failure(approval.error());
    auto awaiting = reader.boolean();
    if (!awaiting) return Result<HostAuth>::failure(awaiting.error());
    auto secret = reader.text(kMaxSecretBytes);
    if (!secret) return Result<HostAuth>::failure(secret.error());
    auto ttl = reader.u32();
    if (!ttl) return Result<HostAuth>::failure(ttl.error());
    if (auto end = reader.require_end(); !end) return Result<HostAuth>::failure(end.error());
    message.accepted = accepted.value();
    message.reason = static_cast<ErrorCode>(reason.value());
    if (message.reason > ErrorCode::TooLarge)
        return Result<HostAuth>::failure({ErrorCode::ProtocolViolation, "code d'erreur inconnu", "wire"});
    message.reason_text = std::move(text).value();
    message.participant = ParticipantId{participant.value()};
    message.host_name = std::move(host_name).value();
    message.session_name = std::move(session_name).value();
    message.approval_required = approval.value();
    if (!control_free(secret.value()))
        return Result<HostAuth>::failure({ErrorCode::InvalidArgument, "secret de reprise invalide", "wire"});
    message.resume_secret = std::move(secret).value();
    message.resume_ttl_ms = ttl.value();
    message.awaiting_approval = awaiting.value();
    if (message.awaiting_approval && message.accepted)
        return Result<HostAuth>::failure({ErrorCode::ProtocolViolation, "accepté et en attente : états contradictoires", "wire"});
    if (!control_free(message.reason_text))
        return Result<HostAuth>::failure({ErrorCode::InvalidArgument, "texte de refus invalide", "wire"});
    return Result<HostAuth>::success(std::move(message));
}

Result<SessionInfo> decode_session_info(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    SessionInfo message;
    auto session = reader.u64();
    if (!session) return Result<SessionInfo>::failure(session.error());
    auto session_name = reader.text(kMaxNameBytes);
    if (!session_name) return Result<SessionInfo>::failure(session_name.error());
    auto host_name = reader.text(kMaxNameBytes);
    if (!host_name) return Result<SessionInfo>::failure(host_name.error());
    auto state = reader.u8();
    if (!state) return Result<SessionInfo>::failure(state.error());
    auto maximum = reader.u32();
    if (!maximum) return Result<SessionInfo>::failure(maximum.error());
    auto started = reader.u64();
    if (!started) return Result<SessionInfo>::failure(started.error());
    auto count = reader.u32();
    if (!count) return Result<SessionInfo>::failure(count.error());
    if (count.value() > kMaxPeers)
        return Result<SessionInfo>::failure({ErrorCode::TooLarge, "liste de participants hors borne", "wire"});
    message.session = SessionId{session.value()};
    message.session_name = std::move(session_name).value();
    message.host_name = std::move(host_name).value();
    message.state = state.value();
    message.max_participants = maximum.value();
    message.started_ms = started.value();
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        PeerInfo peer;
        auto id = reader.u64();
        if (!id) return Result<SessionInfo>::failure(id.error());
        auto name = reader.text(kMaxNameBytes);
        if (!name) return Result<SessionInfo>::failure(name.error());
        auto peer_state = reader.u8();
        if (!peer_state) return Result<SessionInfo>::failure(peer_state.error());
        auto connected = reader.u64();
        if (!connected) return Result<SessionInfo>::failure(connected.error());
        auto latency = reader.u32();
        if (!latency) return Result<SessionInfo>::failure(latency.error());
        auto self = reader.boolean();
        if (!self) return Result<SessionInfo>::failure(self.error());
        peer.id = ParticipantId{id.value()};
        peer.name = std::move(name).value();
        peer.state = peer_state.value();
        peer.connected_ms = connected.value();
        peer.latency_ms = latency.value();
        peer.self = self.value();
        message.peers.push_back(std::move(peer));
    }
    if (!reader.at_end()) {
        auto extended = reader.boolean();
        if (!extended) return Result<SessionInfo>::failure(extended.error());
        if (extended.value()) {
            auto transport = reader.text(kMaxNameBytes * 4);
            if (!transport) return Result<SessionInfo>::failure(transport.error());
            message.transport = std::move(transport).value();
            for (auto& peer : message.peers) {
                auto peer_transport = reader.text(kMaxNameBytes * 4);
                if (!peer_transport) return Result<SessionInfo>::failure(peer_transport.error());
                peer.transport = std::move(peer_transport).value();
            }
        }
        if (!reader.at_end()) {
            auto host = reader.u64();
            if (!host) return Result<SessionInfo>::failure(host.error());
            if (host.value() == 0)
                return Result<SessionInfo>::failure({ErrorCode::ProtocolViolation, "identifiant d'hôte nul", "wire"});
            for (const auto& peer : message.peers)
                if (peer.id.value == host.value())
                    return Result<SessionInfo>::failure({ErrorCode::ProtocolViolation, "hôte listé comme invité", "wire"});
            message.host_participant = ParticipantId{host.value()};
        }
    }
    if (auto end = reader.require_end(); !end) return Result<SessionInfo>::failure(end.error());
    return Result<SessionInfo>::success(std::move(message));
}

Result<ServiceList> decode_service_list(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    ServiceList message;
    auto address = reader.text(kMaxUrlBytes);
    if (!address) return Result<ServiceList>::failure(address.error());
    auto count = reader.u32();
    if (!count) return Result<ServiceList>::failure(count.error());
    if (count.value() > kMaxServices)
        return Result<ServiceList>::failure({ErrorCode::TooLarge, "liste de services hors borne", "wire"});
    message.control_address = std::move(address).value();
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        ServiceInfo service;
        auto key = reader.u64();
        if (!key) return Result<ServiceList>::failure(key.error());
        auto label = reader.text(kMaxNameBytes);
        if (!label) return Result<ServiceList>::failure(label.error());
        auto port = reader.u16();
        if (!port) return Result<ServiceList>::failure(port.error());
        auto url = reader.text(kMaxUrlBytes);
        if (!url) return Result<ServiceList>::failure(url.error());
        auto available = reader.boolean();
        if (!available) return Result<ServiceList>::failure(available.error());
        service.key = key.value();
        service.label = std::move(label).value();
        service.exposed_port = port.value();
        service.url = std::move(url).value();
        service.available = available.value();
        if (!control_free(service.url) || !control_free(service.label))
            return Result<ServiceList>::failure({ErrorCode::InvalidArgument, "service invalide", "wire"});
        message.services.push_back(std::move(service));
    }
    if (auto end = reader.require_end(); !end) return Result<ServiceList>::failure(end.error());
    return Result<ServiceList>::success(std::move(message));
}

Result<ChatText> decode_chat(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    ChatText message;
    auto sender = reader.u64();
    if (!sender) return Result<ChatText>::failure(sender.error());
    auto recipient = reader.u64();
    if (!recipient) return Result<ChatText>::failure(recipient.error());
    auto name = reader.text(kMaxNameBytes);
    if (!name) return Result<ChatText>::failure(name.error());
    auto sent = reader.u64();
    if (!sent) return Result<ChatText>::failure(sent.error());
    auto system = reader.boolean();
    if (!system) return Result<ChatText>::failure(system.error());
    auto body = reader.text(kMaxChatBytes);
    if (!body) return Result<ChatText>::failure(body.error());
    if (auto end = reader.require_end(); !end) return Result<ChatText>::failure(end.error());
    message.sender = ParticipantId{sender.value()};
    message.recipient = ParticipantId{recipient.value()};
    if (message.recipient == message.sender && message.recipient.value != 0)
        return Result<ChatText>::failure({ErrorCode::ProtocolViolation, "destinataire privé identique à l'expéditeur", "wire"});
    message.sender_name = std::move(name).value();
    message.sent_ms = sent.value();
    message.system = system.value();
    message.body = std::move(body).value();
    if (!valid_text(message.body) || !control_free(message.sender_name))
        return Result<ChatText>::failure({ErrorCode::InvalidArgument, "message invalide", "wire"});
    return Result<ChatText>::success(std::move(message));
}

namespace {
constexpr std::size_t kMaxTlsIdentityBytes = 128;

void append_be64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xff));
}

Result<std::vector<std::uint8_t>> keyed(std::span<const std::uint8_t> secret, std::string_view label,
    std::span<const std::uint8_t> message) {
    if (secret.empty())
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::InvalidArgument, "secret d'authentification vide", "wire", 0});
    std::vector<std::uint8_t> keyed_message;
    keyed_message.reserve(label.size() + message.size());
    keyed_message.insert(keyed_message.end(), label.begin(), label.end());
    keyed_message.insert(keyed_message.end(), message.begin(), message.end());
    auto mac = hmac_sha256(secret, keyed_message);
    if (!mac) return Result<std::vector<std::uint8_t>>::failure(mac.error());
    return Result<std::vector<std::uint8_t>>::success(std::move(mac).value());
}
} // namespace

std::vector<std::uint8_t> auth_proof_message(std::span<const std::uint8_t> nonce, std::uint64_t session,
    AuthProofKind kind) {
    std::vector<std::uint8_t> message;
    message.reserve(nonce.size() + 10);
    message.insert(message.end(), nonce.begin(), nonce.end());
    append_be64(message, session);
    message.push_back(static_cast<std::uint8_t>(kind));
    return message;
}

Result<std::vector<std::uint8_t>> compute_auth_proof(std::span<const std::uint8_t> secret,
    std::span<const std::uint8_t> nonce, std::uint64_t session, AuthProofKind kind) {
    if (nonce.size() != kAuthNonceBytes)
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::InvalidArgument, "nonce de défi hors taille", "wire", 0});
    return keyed(secret, "DevLink auth proof v1|", auth_proof_message(nonce, session, kind));
}

Result<std::string> derive_transport_secret(std::span<const std::uint8_t> secret,
    std::span<const std::uint8_t> nonce, std::uint64_t session, const std::string& identity) {
    if (nonce.size() != kAuthNonceBytes)
        return Result<std::string>::failure(
            Error{ErrorCode::InvalidArgument, "nonce de défi hors taille", "wire", 0});
    if (identity.empty() || !control_free(identity))
        return Result<std::string>::failure(
            Error{ErrorCode::InvalidArgument, "identité PSK invalide", "wire", 0});
    std::vector<std::uint8_t> message;
    message.reserve(nonce.size() + identity.size() + 8);
    message.insert(message.end(), nonce.begin(), nonce.end());
    append_be64(message, session);
    message.insert(message.end(), identity.begin(), identity.end());
    auto mac = keyed(secret, "DevLink TLS13 PSK v2|", message);
    if (!mac) return Result<std::string>::failure(mac.error());
    return Result<std::string>::success(to_hex(mac.value()));
}

Result<std::vector<std::uint8_t>> encode_auth_challenge(const AuthChallenge& message) {
    if (message.nonce.size() != kAuthNonceBytes)
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::InvalidArgument, "nonce de défi hors taille", "wire", 0});
    PayloadWriter writer;
    writer.blob(message.nonce);
    return Result<std::vector<std::uint8_t>>::success(writer.take());
}

Result<AuthChallenge> decode_auth_challenge(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    AuthChallenge message;
    auto nonce = reader.blob(kAuthNonceBytes);
    if (!nonce) return Result<AuthChallenge>::failure(nonce.error());
    if (auto end = reader.require_end(); !end) return Result<AuthChallenge>::failure(end.error());
    message.nonce = std::move(nonce).value();
    if (message.nonce.size() != kAuthNonceBytes)
        return Result<AuthChallenge>::failure({ErrorCode::ProtocolViolation,
            "défi : nonce de taille inattendue", "wire"});
    return Result<AuthChallenge>::success(std::move(message));
}

Result<std::vector<std::uint8_t>> encode_auth_proof(const AuthProof& message) {
    if (message.invite_proof.empty() && message.resume_proof.empty())
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::InvalidArgument, "réponse au défi sans aucune preuve", "wire", 0});
    for (const auto& proof : {message.invite_proof, message.resume_proof}) {
        if (!proof.empty() && proof.size() != kAuthProofBytes)
            return Result<std::vector<std::uint8_t>>::failure(
                Error{ErrorCode::InvalidArgument, "preuve hors taille", "wire", 0});
    }
    PayloadWriter writer;
    writer.blob(message.invite_proof);
    writer.blob(message.resume_proof);
    return Result<std::vector<std::uint8_t>>::success(writer.take());
}

Result<AuthProof> decode_auth_proof(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    AuthProof message;
    auto invite = reader.blob(kAuthProofBytes);
    if (!invite) return Result<AuthProof>::failure(invite.error());
    auto resume = reader.blob(kAuthProofBytes);
    if (!resume) return Result<AuthProof>::failure(resume.error());
    if (auto end = reader.require_end(); !end) return Result<AuthProof>::failure(end.error());
    message.invite_proof = std::move(invite).value();
    message.resume_proof = std::move(resume).value();
    for (const auto& proof : {message.invite_proof, message.resume_proof}) {
        if (!proof.empty() && proof.size() != kAuthProofBytes)
            return Result<AuthProof>::failure({ErrorCode::ProtocolViolation,
                "preuve de taille inattendue", "wire"});
    }
    if (message.invite_proof.empty() && message.resume_proof.empty())
        return Result<AuthProof>::failure({ErrorCode::ProtocolViolation,
            "réponse au défi sans aucune preuve", "wire"});
    return Result<AuthProof>::success(std::move(message));
}

Result<std::vector<std::uint8_t>> encode_tls_upgrade(const TlsUpgradeNotice& message) {
    if (message.identity.empty() || !control_free(message.identity))
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::InvalidArgument, "upgrade tls : identité PSK invalide", "wire", 0});
    if (message.identity.size() > kMaxTlsIdentityBytes)
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::TooLarge, "upgrade tls : identité hors borne", "wire", 0});
    if (message.nonce.size() != kAuthNonceBytes)
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::InvalidArgument, "upgrade tls : nonce hors taille", "wire", 0});
    PayloadWriter writer;
    writer.text(message.identity);
    writer.u8(static_cast<std::uint8_t>(message.secret_kind));
    writer.blob(message.nonce);
    return Result<std::vector<std::uint8_t>>::success(writer.take());
}

Result<TlsUpgradeNotice> decode_tls_upgrade(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    TlsUpgradeNotice message;
    auto identity = reader.text(kMaxTlsIdentityBytes);
    if (!identity) return Result<TlsUpgradeNotice>::failure(identity.error());
    auto kind = reader.u8();
    if (!kind) return Result<TlsUpgradeNotice>::failure(kind.error());
    if (kind.value() > static_cast<std::uint8_t>(AuthProofKind::ResumeSecret))
        return Result<TlsUpgradeNotice>::failure({ErrorCode::ProtocolViolation,
            "upgrade tls : nature de secret inconnue", "wire"});
    auto nonce = reader.blob(kAuthNonceBytes);
    if (!nonce) return Result<TlsUpgradeNotice>::failure(nonce.error());
    if (auto end = reader.require_end(); !end) return Result<TlsUpgradeNotice>::failure(end.error());
    message.identity = std::move(identity).value();
    message.secret_kind = static_cast<AuthProofKind>(kind.value());
    message.nonce = std::move(nonce).value();
    if (message.identity.empty())
        return Result<TlsUpgradeNotice>::failure({ErrorCode::ProtocolViolation,
            "upgrade tls : identité vide", "wire"});
    if (!control_free(message.identity))
        return Result<TlsUpgradeNotice>::failure({ErrorCode::InvalidArgument,
            "upgrade tls : identité invalide", "wire"});
    if (message.nonce.size() != kAuthNonceBytes)
        return Result<TlsUpgradeNotice>::failure({ErrorCode::ProtocolViolation,
            "upgrade tls : nonce de taille inattendue", "wire"});
    return Result<TlsUpgradeNotice>::success(std::move(message));
}

Result<ByeNotice> decode_bye(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    ByeNotice message;
    auto reason = reader.u16();
    if (!reason) return Result<ByeNotice>::failure(reason.error());
    auto text = reader.text(kMaxReasonBytes);
    if (!text) return Result<ByeNotice>::failure(text.error());
    if (auto end = reader.require_end(); !end) return Result<ByeNotice>::failure(end.error());
    message.reason = static_cast<ErrorCode>(reason.value());
    if (message.reason > ErrorCode::TooLarge)
        return Result<ByeNotice>::failure({ErrorCode::ProtocolViolation, "code de départ inconnu", "wire"});
    message.text = std::move(text).value();
    if (!control_free(message.text))
        return Result<ByeNotice>::failure({ErrorCode::InvalidArgument, "texte de départ invalide", "wire"});
    return Result<ByeNotice>::success(std::move(message));
}

Result<ErrorNotice> decode_error(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    ErrorNotice message;
    auto reason = reader.u16();
    if (!reason) return Result<ErrorNotice>::failure(reason.error());
    auto text = reader.text(kMaxReasonBytes);
    if (!text) return Result<ErrorNotice>::failure(text.error());
    if (auto end = reader.require_end(); !end) return Result<ErrorNotice>::failure(end.error());
    message.reason = static_cast<ErrorCode>(reason.value());
    if (message.reason > ErrorCode::TooLarge)
        return Result<ErrorNotice>::failure({ErrorCode::ProtocolViolation, "code d'erreur inconnu", "wire"});
    message.text = std::move(text).value();
    if (!control_free(message.text))
        return Result<ErrorNotice>::failure({ErrorCode::InvalidArgument, "texte d'erreur invalide", "wire"});
    return Result<ErrorNotice>::success(std::move(message));
}

std::vector<std::uint8_t> encode(const MediaOffer& m) {
    PayloadWriter w;
    w.u16(m.port);
    w.text(m.identity);
    w.text(m.key);
    w.u8(m.mode);
    w.u32(m.viewport_width);
    w.u32(m.viewport_height);
    return w.take();
}

Result<MediaOffer> decode_media_offer(std::span<const std::uint8_t> payload) {
    PayloadReader r(payload);
    auto port = r.u16();
    auto identity = r.text(kMaxNameBytes + 32);
    auto key = r.text(kMaxSecretBytes);
    auto mode = r.u8();
    auto w = r.u32();
    auto h = r.u32();
    if (!port || !identity || !key || !mode || !w || !h || !r.require_end())
        return Result<MediaOffer>::failure({ErrorCode::ProtocolViolation, "offre média tronquée", "wire"});
    const auto& k = key.value();
    const bool hex = k.size() == 64 && std::all_of(k.begin(), k.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (port.value() == 0 || identity.value().empty() || !hex || mode.value() < 1 || mode.value() > 3 ||
        w.value() == 0 || h.value() == 0 || w.value() > 4096 || h.value() > 2160)
        return Result<MediaOffer>::failure({ErrorCode::ProtocolViolation, "offre média invalide", "wire"});
    MediaOffer m;
    m.port = port.value();
    m.identity = std::move(identity).value();
    m.key = k;
    m.mode = mode.value();
    m.viewport_width = w.value();
    m.viewport_height = h.value();
    return Result<MediaOffer>::success(std::move(m));
}

Result<Heartbeat> decode_heartbeat(std::span<const std::uint8_t> data) {
    PayloadReader reader(data);
    Heartbeat message;
    auto monotonic = reader.u64();
    if (!monotonic) return Result<Heartbeat>::failure(monotonic.error());
    auto echo = reader.u64();
    if (!echo) return Result<Heartbeat>::failure(echo.error());
    if (auto end = reader.require_end(); !end) return Result<Heartbeat>::failure(end.error());
    message.monotonic_ms = monotonic.value();
    message.echo = echo.value();
    return Result<Heartbeat>::success(message);
}

Result<std::vector<std::uint8_t>> frame(model::MessageType type, SessionId session, ParticipantId participant,
    std::uint64_t sequence, std::span<const std::uint8_t> payload) {
    model::Envelope envelope;
    envelope.type = type;
    envelope.session = session;
    envelope.participant = participant;
    envelope.stream_id = static_cast<std::uint32_t>(model::LogicalStream::Control);
    envelope.sequence = sequence;
    envelope.timestamp_monotonic_ms = 0;
    envelope.flags = 0;
    envelope.payload.assign(payload.begin(), payload.end());
    return model::encode_envelope(envelope);
}

FrameStatus inspect_frame(std::span<const std::uint8_t> buffer, std::size_t& total_bytes, Error& reason) {
    total_bytes = 0;
    reason = Error{};
    if (buffer.size() < kHeaderBytes) return FrameStatus::Incomplete;
    if (read_be(buffer, 0, 4) != kFrameMagic) {
        reason = {ErrorCode::ProtocolViolation, "magie de trame absente", "wire"};
        return FrameStatus::Invalid;
    }
    const auto major = static_cast<std::uint16_t>(read_be(buffer, 4, 2));
    const auto minor = static_cast<std::uint16_t>(read_be(buffer, 6, 2));
    if (major != model::kProtocolMajor || minor > model::kProtocolMinor) {
        reason = {ErrorCode::UnsupportedVersion, "version de protocole non négociable", "wire"};
        return FrameStatus::Invalid;
    }
    const auto type_value = static_cast<std::uint16_t>(read_be(buffer, 8, 2));
    if (read_be(buffer, 10, 2) != 0) {
        reason = {ErrorCode::ProtocolViolation, "champ réservé non nul", "wire"};
        return FrameStatus::Invalid;
    }
    const auto type = static_cast<model::MessageType>(type_value);
    const auto maximum = model::max_payload_size(type);
    const auto declared = static_cast<std::size_t>(read_be(buffer, 48, 4));
    if (maximum == 0 || declared > maximum) {
        reason = {ErrorCode::ProtocolViolation, "type inconnu ou charge utile hors borne", "wire"};
        return FrameStatus::Invalid;
    }
    if (kHeaderBytes + declared > kMaxFrameBytes) {
        reason = {ErrorCode::TooLarge, "trame au-delà de 64 Kio", "wire"};
        return FrameStatus::Invalid;
    }
    total_bytes = kHeaderBytes + declared;
    return buffer.size() < total_bytes ? FrameStatus::Incomplete : FrameStatus::Complete;
}

Result<void> FrameAssembler::feed(std::span<const std::uint8_t> data) {
    if (buffer_.size() + data.size() > kAssemblerCapBytes) {
        buffer_.clear();
        return Result<void>::failure({ErrorCode::TooLarge, "flux de contrôle au-delà de 256 Kio sans trame complète", "wire"});
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    return Result<void>::success();
}

Result<model::Envelope> FrameAssembler::next() {
    std::size_t total = 0;
    Error reason{};
    const auto status = inspect_frame({buffer_.data(), buffer_.size()}, total, reason);
    if (status == FrameStatus::Invalid) {
        buffer_.clear();
        return Result<model::Envelope>::failure(reason);
    }
    if (status == FrameStatus::Incomplete || total > max_frame_bytes_)
        return Result<model::Envelope>::failure({ErrorCode::WouldBlock, "trame incomplète", "wire"});
    auto decoded = model::decode_envelope({buffer_.data(), total});
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    if (!decoded) return Result<model::Envelope>::failure(decoded.error());
    return Result<model::Envelope>::success(std::move(decoded).value());
}

} // namespace plpt::net
