#include "envelope.hpp"
namespace plpt::model {
std::uint32_t max_payload_size(MessageType type) {
    switch (type) {
    case MessageType::Hello: case MessageType::Auth: case MessageType::VideoControl: case MessageType::Error: return 4096;
    case MessageType::SessionControl: case MessageType::Chat: return 8192;
    case MessageType::InputKeyboard: case MessageType::InputPointer: return 1024;
    case MessageType::VideoFrame: return 256 * 1024;
    case MessageType::Presence: return 2048;
    case MessageType::Feedback: return 16384;
    case MessageType::Heartbeat: return 256;
    case MessageType::SessionState: return 16384; // 64 participants nommés bornent ce plafond.
    case MessageType::ServiceList: return 8192;   // 64 services partagés au plus.
    case MessageType::Bye: return 1024;
    }
    return 0;
}
Result<void> negotiate_version(std::uint16_t major, std::uint16_t minor) {
    if (major != kProtocolMajor || minor > kProtocolMinor)
        return Result<void>::failure({ErrorCode::UnsupportedVersion, "version non négociée", "protocol"});
    return Result<void>::success();
}
namespace {
Error malformed() { return {ErrorCode::ProtocolViolation, "enveloppe invalide", "protocol"}; }
Result<void> validate_header(const Envelope& e, std::size_t size) {
    if (auto v = negotiate_version(e.protocol_major, e.protocol_minor); !v) return v;
    const auto maximum = max_payload_size(e.type);
    if (maximum == 0 || size > maximum || e.flags != 0 || e.stream_id > 5)
        return Result<void>::failure(malformed());
    if (e.type == MessageType::Hello) {
        if (e.session.value != 0 || e.participant.value != 0) return Result<void>::failure(malformed());
    } else if (e.session.value == 0) return Result<void>::failure(malformed());
    if (e.type != MessageType::Hello && e.type != MessageType::Auth && (e.participant.value == 0 || e.sequence == 0))
        return Result<void>::failure(malformed());
    return Result<void>::success();
}
void put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned bytes) {
    for (unsigned n = bytes; n > 0; --n) out.push_back(static_cast<std::uint8_t>(value >> ((n - 1) * 8)));
}
std::uint64_t get(std::span<const std::uint8_t> in, std::size_t& offset, unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned n = 0; n < bytes; ++n) value = (value << 8) | in[offset++];
    return value;
}
}
Result<void> validate_envelope(const Envelope& e) { return validate_header(e, e.payload.size()); }
Result<std::vector<std::uint8_t>> encode_envelope(const Envelope& e) {
    if (auto v = validate_envelope(e); !v) return Result<std::vector<std::uint8_t>>::failure(v.error());
    std::vector<std::uint8_t> out;
    out.reserve(kEnvelopeHeaderBytes + e.payload.size());
    put(out, 0x504C5054, 4); // ASCII PLPT
    put(out, e.protocol_major, 2); put(out, e.protocol_minor, 2);
    put(out, static_cast<std::uint16_t>(e.type), 2); put(out, 0, 2);
    put(out, e.session.value, 8); put(out, e.participant.value, 8); put(out, e.stream_id, 4);
    put(out, e.sequence, 8); put(out, e.timestamp_monotonic_ms, 8);
    put(out, e.payload.size(), 4); put(out, e.flags, 4);
    out.insert(out.end(), e.payload.begin(), e.payload.end());
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}
Result<Envelope> decode_envelope(std::span<const std::uint8_t> in) {
    if (in.size() < kEnvelopeHeaderBytes) return Result<Envelope>::failure(malformed());
    std::size_t cursor = 0;
    if (get(in, cursor, 4) != 0x504C5054) return Result<Envelope>::failure(malformed());
    Envelope e;
    e.protocol_major = static_cast<std::uint16_t>(get(in, cursor, 2));
    e.protocol_minor = static_cast<std::uint16_t>(get(in, cursor, 2));
    e.type = static_cast<MessageType>(get(in, cursor, 2));
    if (get(in, cursor, 2) != 0) return Result<Envelope>::failure(malformed());
    e.session = SessionId{get(in, cursor, 8)}; e.participant = ParticipantId{get(in, cursor, 8)};
    e.stream_id = static_cast<std::uint32_t>(get(in, cursor, 4));
    e.sequence = get(in, cursor, 8); e.timestamp_monotonic_ms = get(in, cursor, 8);
    const auto size = get(in, cursor, 4);
    e.flags = static_cast<std::uint32_t>(get(in, cursor, 4));
    if (size != in.size() - kEnvelopeHeaderBytes) return Result<Envelope>::failure(malformed());
    if (auto v = validate_header(e, static_cast<std::size_t>(size)); !v) return Result<Envelope>::failure(v.error());
    e.payload.assign(in.begin() + kEnvelopeHeaderBytes, in.end());
    return Result<Envelope>::success(std::move(e));
}
} // namespace plpt::model
