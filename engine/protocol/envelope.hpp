#pragma once
#include <span>
#include <vector>
#include "../core/types.hpp"
#include "../core/result.hpp"
namespace plpt::model {
inline constexpr std::uint16_t kProtocolMajor = 1, kProtocolMinor = 0;
inline constexpr std::size_t kEnvelopeHeaderBytes = 56;
enum class MessageType : std::uint16_t {
    Hello = 1, Auth, SessionControl, InputKeyboard, InputPointer, VideoFrame,
    VideoControl, Chat, Presence, Feedback, Heartbeat, Error
};
struct Envelope {
    std::uint16_t protocol_major = kProtocolMajor, protocol_minor = kProtocolMinor;
    MessageType type = MessageType::Error;
    SessionId session{};
    ParticipantId participant{};
    std::uint32_t stream_id = 0;
    std::uint64_t sequence = 0, timestamp_monotonic_ms = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> payload;
};
std::uint32_t max_payload_size(MessageType);
Result<void> validate_envelope(const Envelope&);
Result<void> negotiate_version(std::uint16_t major, std::uint16_t minor);
Result<std::vector<std::uint8_t>> encode_envelope(const Envelope&);
// Valide le header, le type et la longueur AVANT d'allouer le payload.
// Retourne une erreur sur troncature et sur octets surnuméraires.
Result<Envelope> decode_envelope(std::span<const std::uint8_t>);
} // namespace plpt::model
