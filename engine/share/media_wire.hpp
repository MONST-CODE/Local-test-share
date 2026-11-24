#pragma once
// DevLink — engine/share/media_wire.hpp — media channel records (ADR-018 §5).
//
// One TCP connection per guest, separate from the control plane so that large
// frames never delay chat/heartbeats. Framing: u32 big-endian length, then the
// record (first byte = kind). Every length is checked BEFORE allocation.
//
//   clear, guest -> host, exactly once:   Hello   (identifies the grant only)
//   then TLS 1.3 PSK with the per-participant key issued over the already
//   encrypted control channel; every following record is inside TLS:
//   host -> guest : Frame, State, Closing
//   guest -> host : Input, Control
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../browser/remote_frame.hpp"
#include "../core/result.hpp"
#include "../core/types.hpp"
#include "share_policy.hpp"

namespace plpt::share {

inline constexpr std::uint16_t kMediaChannelVersion = 1;
inline constexpr std::uint32_t kMediaHelloMagic = 0x444C4D48U; // "DLMH"
inline constexpr std::size_t kMaxHelloRecord = 64;
inline constexpr std::size_t kMaxControlRecord = 4096;
inline constexpr std::size_t kMaxMediaRecord = media::kFrameHeaderBytes + media::kMaxCompressedPayloadBytes + 64;

enum class RecordKind : std::uint8_t { Hello = 1, Frame = 2, Input = 3, Control = 4, State = 5, Closing = 6 };
enum class ControlAction : std::uint8_t { RequestControl = 1, ReleaseControl = 2 };

struct MediaHello {
    std::uint64_t session = 0;
    std::uint64_t participant = 0;
};

struct FrameRecord {
    std::uint64_t last_input_seq = 0; // highest input injected before capture
    media::VideoFrame frame;
};

struct InputRecord {
    std::uint64_t input_seq = 0;
    media::InputEvent event;
};

struct StateRecord {
    ShareMode mode = ShareMode::ObserveOnly;
    std::uint64_t you = 0;
    std::uint64_t controller = 0;        // Mode B holder, 0 = nobody
    std::uint64_t lease_epoch = 0;
    bool you_may_input = false;
    bool you_waiting = false;            // queued for control
    bool paused = false;
    bool context_ready = false;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint32_t last_rejection = 0;    // InputDecision of the last refused input
    std::string note;                    // short, displayable, never a secret
};

struct ClosingRecord {
    ErrorCode reason = ErrorCode::Ok;
    std::string text;
};

// The PSK identity of a media channel is public and derivable by both sides.
inline std::string media_identity(std::uint64_t session, std::uint64_t participant) {
    return "devlink-media-s" + std::to_string(session) + "-p" + std::to_string(participant);
}

// ---- encoders: return the complete record INCLUDING the u32 length prefix ----
std::vector<std::uint8_t> encode_hello(const MediaHello&);
Result<std::vector<std::uint8_t>> encode_frame(const FrameRecord&);
Result<std::vector<std::uint8_t>> encode_input(const InputRecord&);
std::vector<std::uint8_t> encode_control(ControlAction);
std::vector<std::uint8_t> encode_state(const StateRecord&);
std::vector<std::uint8_t> encode_closing(const ClosingRecord&);

// ---- decoders: take the record body (after the length prefix) ----
Result<MediaHello> decode_hello(std::span<const std::uint8_t>);
Result<FrameRecord> decode_frame(std::span<const std::uint8_t>);
Result<InputRecord> decode_input(std::span<const std::uint8_t>);
Result<ControlAction> decode_control(std::span<const std::uint8_t>);
Result<StateRecord> decode_state(std::span<const std::uint8_t>);
Result<ClosingRecord> decode_closing(std::span<const std::uint8_t>);

// Incremental reader for a byte stream; bounded by `max_record`.
class RecordAssembler {
public:
    explicit RecordAssembler(std::size_t max_record) : max_record_(max_record) {}
    // Appends bytes; fails (and must not be fed again) on an oversized length.
    Result<void> feed(std::span<const std::uint8_t> bytes);
    // Next complete record body, if any.
    std::optional<std::vector<std::uint8_t>> next();
    std::size_t buffered() const { return buffer_.size() - offset_; }
    // True once an invalid length was seen: the stream must be closed.
    bool failed() const { return failed_; }
private:
    std::size_t max_record_;
    bool failed_ = false;
    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
};

} // namespace plpt::share
