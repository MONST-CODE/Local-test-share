#pragma once
#include <span>
#include <vector>
#include "../core/types.hpp"
#include "../core/result.hpp"
namespace plpt::model {
// 1.1 ajoute les messages du partage LAN (SessionState, ServiceList, Bye).
// Un pair 1.0 reste acceptable : les mineures sont rétrocompatibles, jamais les majeures.
inline constexpr std::uint16_t kProtocolMajor = 1, kProtocolMinor = 1;
inline constexpr std::size_t kEnvelopeHeaderBytes = 56;
// Flux logiques multiplexés dans stream_id. Un seul est réellement utilisé par le
// partage LAN (Control) ; les autres restent réservés et jamais acceptés en vrac.
enum class LogicalStream : std::uint8_t { Control, InputKeyboard, InputPointer, Video, Messaging, Presence };
enum class MessageType : std::uint16_t {
    Hello = 1, Auth, SessionControl, InputKeyboard, InputPointer, VideoFrame,
    VideoControl, Chat, Presence, Feedback, Heartbeat, Error,
    // Partage LAN réel (protocole 1.1) : état de session, services autorisés, départ.
    SessionState = 13, ServiceList = 14, Bye = 15
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
// Accepte la majeure courante et toute mineure <= à la mineure courante.
Result<void> negotiate_version(std::uint16_t major, std::uint16_t minor);
Result<std::vector<std::uint8_t>> encode_envelope(const Envelope&);
// Valide le header, le type et la longueur AVANT d'allouer le payload.
// Retourne une erreur sur troncature et sur octets surnuméraires.
Result<Envelope> decode_envelope(std::span<const std::uint8_t>);
} // namespace plpt::model
