// PLPT — engine/net/wire.cpp — sérialisation stricte des messages LAN.
#include "wire.hpp"

#include <algorithm>
#include <cstring>
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
    if (auto end = reader.require_end(); !end) return Result<GuestHello>::failure(end.error());
    message.guest_name = std::move(name).value();
    message.invite_code = std::move(code).value();
    message.client = std::move(client).value();
    if (!control_free(message.guest_name) || !valid_text(message.guest_name))
        return Result<GuestHello>::failure({ErrorCode::InvalidArgument, "nom d'invité invalide", "wire"});
    if (!control_free(message.invite_code))
        return Result<GuestHello>::failure({ErrorCode::InvalidArgument, "code d'invitation invalide", "wire"});
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
