#pragma once
// DevLink — portable media/input wire primitives.
// These codecs define a bounded, explicit wire representation; they are not yet
// connected to a WebView2 capture or a production media transport.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../core/result.hpp"
#include "../core/text.hpp"
#include "../core/types.hpp"

namespace plpt::media {

inline constexpr std::uint16_t kMediaProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 72;
inline constexpr std::size_t kInputEventHeaderBytes = 24;
inline constexpr std::size_t kMaxInputTextBytes = 32;
inline constexpr std::uint32_t kMaxFrameWidth = 4096;
inline constexpr std::uint32_t kMaxFrameHeight = 2160;
inline constexpr std::size_t kMaxFramePixels = 8'847'360; // 4096x2160
inline constexpr std::size_t kMaxFramePayloadBytes = kMaxFramePixels * 4;
inline constexpr std::uint16_t kFrameFlagKeyFrame = 1;

// Version 1 deliberately supports one self-contained format. A codec is added
// only when its dependency, limits and decoder are implemented and tested.
enum class FrameCodec : std::uint8_t { Raw = 0 };
enum class PixelFormat : std::uint8_t { RGBA8 = 1 };

struct FrameHeader {
    std::uint16_t version = kMediaProtocolVersion;
    FrameCodec codec = FrameCodec::Raw;
    PixelFormat pixel_format = PixelFormat::RGBA8;
    std::uint16_t flags = kFrameFlagKeyFrame;
    SessionId session{};
    ParticipantId participant{};
    std::uint32_t stream_id = 0;
    ContextId context{};
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;
};

struct VideoFrame {
    FrameHeader header{};
    std::vector<std::uint8_t> pixels; // tightly packed RGBA8, one complete frame
};

enum class InputEventType : std::uint8_t {
    MouseMove = 1,
    MouseDown = 2,
    MouseUp = 3,
    MouseWheel = 4,
    KeyDown = 5,
    KeyUp = 6,
    TextInput = 7,
    FocusLost = 8
};

struct InputEvent {
    InputEventType type = InputEventType::MouseMove;
    float x_norm = 0.0f; // finite [0,1], quantized to unsigned 16-bit on wire
    float y_norm = 0.0f;
    std::uint8_t button = 0; // 1..5 for mouse down/up; otherwise zero
    std::uint16_t modifiers = 0; // Shift/Ctrl/Alt/Meta/CapsLock bits 0..4
    std::int16_t wheel_delta = 0;
    std::uint32_t key_code = 0; // virtual key code, 1..0xffff for key events
    std::string text; // UTF-8, max 32 bytes; never logged by this codec
};

namespace detail {
inline constexpr std::uint32_t kFrameMagic = 0x444C5646U; // "DLVF"
inline constexpr std::uint32_t kInputMagic = 0x444C5649U; // "DLVI"

inline Error protocol_error(std::string message) {
    return {ErrorCode::ProtocolViolation, std::move(message), "media"};
}
inline Error invalid_argument(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message), "media"};
}

template <typename UInt>
inline void write_be(std::span<std::uint8_t> out, std::size_t offset, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        const auto shift = static_cast<unsigned>((sizeof(UInt) - 1 - i) * 8);
        out[offset + i] = static_cast<std::uint8_t>((value >> shift) & static_cast<UInt>(0xff));
    }
}

template <typename UInt>
inline UInt read_be(std::span<const std::uint8_t> in, std::size_t offset) {
    static_assert(std::is_unsigned_v<UInt>);
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i)
        value = static_cast<UInt>((value << 8) | static_cast<UInt>(in[offset + i]));
    return value;
}

inline bool valid_coordinates(float x, float y) {
    return std::isfinite(x) && std::isfinite(y) && x >= 0.0f && x <= 1.0f &&
        y >= 0.0f && y <= 1.0f;
}
inline std::uint16_t quantize_coordinate(float value) {
    return static_cast<std::uint16_t>(std::lround(value * 65535.0f));
}

inline Result<void> validate_input(const InputEvent& event) {
    if (event.modifiers & ~std::uint16_t{0x001f})
        return Result<void>::failure(invalid_argument("bits de modificateur réservés"));
    const bool pointer = event.type == InputEventType::MouseMove || event.type == InputEventType::MouseDown ||
        event.type == InputEventType::MouseUp || event.type == InputEventType::MouseWheel;
    if (pointer && !valid_coordinates(event.x_norm, event.y_norm))
        return Result<void>::failure(invalid_argument("coordonnées souris hors borne"));
    if (!pointer && (event.x_norm != 0.0f || event.y_norm != 0.0f))
        return Result<void>::failure(invalid_argument("coordonnées inattendues pour cet événement"));

    switch (event.type) {
    case InputEventType::MouseMove:
        if (event.button != 0 || event.wheel_delta != 0 || event.key_code != 0 || !event.text.empty()) break;
        return Result<void>::success();
    case InputEventType::MouseDown:
    case InputEventType::MouseUp:
        if (event.button >= 1 && event.button <= 5 && event.wheel_delta == 0 &&
            event.key_code == 0 && event.text.empty()) return Result<void>::success();
        break;
    case InputEventType::MouseWheel:
        if (event.button == 0 && event.key_code == 0 && event.text.empty()) return Result<void>::success();
        break;
    case InputEventType::KeyDown:
    case InputEventType::KeyUp:
        if (event.button == 0 && event.wheel_delta == 0 && event.key_code >= 1 &&
            event.key_code <= 0xffff && event.text.empty()) return Result<void>::success();
        break;
    case InputEventType::TextInput:
        if (event.button == 0 && event.wheel_delta == 0 && event.key_code == 0 &&
            !event.text.empty() && event.text.size() <= kMaxInputTextBytes && valid_text(event.text))
            return Result<void>::success();
        break;
    case InputEventType::FocusLost:
        if (event.button == 0 && event.wheel_delta == 0 && event.key_code == 0 && event.text.empty())
            return Result<void>::success();
        break;
    }
    return Result<void>::failure(invalid_argument("champs incompatibles avec le type d'événement"));
}

inline Result<void> validate_frame(const VideoFrame& frame) {
    const auto& h = frame.header;
    if (h.version != kMediaProtocolVersion)
        return Result<void>::failure({ErrorCode::UnsupportedVersion, "version média non prise en charge", "media"});
    if (h.codec != FrameCodec::Raw || h.pixel_format != PixelFormat::RGBA8)
        return Result<void>::failure({ErrorCode::Unsupported, "codec ou format pixel non pris en charge", "media"});
    if (h.flags != kFrameFlagKeyFrame)
        return Result<void>::failure(protocol_error("flags de frame réservés ou invalides"));
    if (h.context.value == 0 || h.width == 0 || h.height == 0 ||
        h.width > kMaxFrameWidth || h.height > kMaxFrameHeight)
        return Result<void>::failure(invalid_argument("identifiant de contexte ou dimensions invalides"));
    const std::uint64_t expected_stride = static_cast<std::uint64_t>(h.width) * 4;
    const std::uint64_t expected_size = expected_stride * h.height;
    if (expected_size > kMaxFramePayloadBytes || h.stride_bytes != expected_stride ||
        frame.pixels.size() != expected_size)
        return Result<void>::failure(invalid_argument("stride ou taille de frame RGBA invalide"));
    return Result<void>::success();
}
} // namespace detail

inline Result<void> validate_video_frame(const VideoFrame& frame) {
    return detail::validate_frame(frame);
}

// Serializes to a fixed, network-endian header followed by tightly packed RGBA8.
// Header fields: magic, version/header length, codec/pixel/flags, session,
// participant, stream, context, sequence, monotonic timestamp, dimensions,
// stride and payload length. No native struct representation crosses the wire.
inline Result<std::vector<std::uint8_t>> serialize_video_frame(const VideoFrame& frame) {
    auto valid = detail::validate_frame(frame);
    if (!valid) return Result<std::vector<std::uint8_t>>::failure(valid.error());
    const std::size_t total = kFrameHeaderBytes + frame.pixels.size();
    std::vector<std::uint8_t> out(total, 0);
    const std::span<std::uint8_t> bytes(out);
    detail::write_be<std::uint32_t>(bytes, 0, detail::kFrameMagic);
    detail::write_be<std::uint16_t>(bytes, 4, frame.header.version);
    detail::write_be<std::uint16_t>(bytes, 6, static_cast<std::uint16_t>(kFrameHeaderBytes));
    bytes[8] = static_cast<std::uint8_t>(frame.header.codec);
    bytes[9] = static_cast<std::uint8_t>(frame.header.pixel_format);
    detail::write_be<std::uint16_t>(bytes, 10, frame.header.flags);
    detail::write_be<std::uint64_t>(bytes, 12, frame.header.session.value);
    detail::write_be<std::uint64_t>(bytes, 20, frame.header.participant.value);
    detail::write_be<std::uint32_t>(bytes, 28, frame.header.stream_id);
    detail::write_be<std::uint64_t>(bytes, 32, frame.header.context.value);
    detail::write_be<std::uint64_t>(bytes, 40, frame.header.sequence);
    detail::write_be<std::uint64_t>(bytes, 48, frame.header.timestamp_ns);
    detail::write_be<std::uint32_t>(bytes, 56, frame.header.width);
    detail::write_be<std::uint32_t>(bytes, 60, frame.header.height);
    detail::write_be<std::uint32_t>(bytes, 64, frame.header.stride_bytes);
    detail::write_be<std::uint32_t>(bytes, 68, static_cast<std::uint32_t>(frame.pixels.size()));
    std::copy(frame.pixels.begin(), frame.pixels.end(), out.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes));
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}

inline Result<VideoFrame> deserialize_video_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kFrameHeaderBytes)
        return Result<VideoFrame>::failure(detail::protocol_error("en-tête de frame tronqué"));
    if (detail::read_be<std::uint32_t>(bytes, 0) != detail::kFrameMagic)
        return Result<VideoFrame>::failure(detail::protocol_error("magic de frame invalide"));
    const auto version = detail::read_be<std::uint16_t>(bytes, 4);
    if (version != kMediaProtocolVersion)
        return Result<VideoFrame>::failure({ErrorCode::UnsupportedVersion, "version média non prise en charge", "media"});
    if (detail::read_be<std::uint16_t>(bytes, 6) != kFrameHeaderBytes)
        return Result<VideoFrame>::failure(detail::protocol_error("taille d'en-tête média invalide"));
    const auto payload_size = detail::read_be<std::uint32_t>(bytes, 68);
    if (payload_size > kMaxFramePayloadBytes)
        return Result<VideoFrame>::failure({ErrorCode::TooLarge, "payload vidéo au-delà de la limite", "media"});
    if (bytes.size() != kFrameHeaderBytes + static_cast<std::size_t>(payload_size))
        return Result<VideoFrame>::failure(detail::protocol_error("taille de payload vidéo incohérente"));

    VideoFrame frame;
    frame.header.version = version;
    frame.header.codec = static_cast<FrameCodec>(bytes[8]);
    frame.header.pixel_format = static_cast<PixelFormat>(bytes[9]);
    frame.header.flags = detail::read_be<std::uint16_t>(bytes, 10);
    frame.header.session = SessionId{detail::read_be<std::uint64_t>(bytes, 12)};
    frame.header.participant = ParticipantId{detail::read_be<std::uint64_t>(bytes, 20)};
    frame.header.stream_id = detail::read_be<std::uint32_t>(bytes, 28);
    frame.header.context = ContextId{detail::read_be<std::uint64_t>(bytes, 32)};
    frame.header.sequence = detail::read_be<std::uint64_t>(bytes, 40);
    frame.header.timestamp_ns = detail::read_be<std::uint64_t>(bytes, 48);
    frame.header.width = detail::read_be<std::uint32_t>(bytes, 56);
    frame.header.height = detail::read_be<std::uint32_t>(bytes, 60);
    frame.header.stride_bytes = detail::read_be<std::uint32_t>(bytes, 64);
    frame.pixels.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes), bytes.end());
    auto valid = detail::validate_frame(frame);
    if (!valid) return Result<VideoFrame>::failure(valid.error());
    return Result<VideoFrame>::success(std::move(frame));
}

// Versioned input packet. Coordinates use unsigned Q0.16 normalized values;
// text is bounded UTF-8 and remains a variable-length field.
inline Result<std::vector<std::uint8_t>> serialize_input_event(const InputEvent& event) {
    auto valid = detail::validate_input(event);
    if (!valid) return Result<std::vector<std::uint8_t>>::failure(valid.error());
    const std::size_t total = kInputEventHeaderBytes + event.text.size();
    std::vector<std::uint8_t> out(total, 0);
    const std::span<std::uint8_t> bytes(out);
    detail::write_be<std::uint32_t>(bytes, 0, detail::kInputMagic);
    bytes[4] = 1; // input protocol version
    bytes[5] = static_cast<std::uint8_t>(event.type);
    bytes[6] = event.button;
    detail::write_be<std::uint16_t>(bytes, 8, event.modifiers);
    detail::write_be<std::uint16_t>(bytes, 10, detail::quantize_coordinate(event.x_norm));
    detail::write_be<std::uint16_t>(bytes, 12, detail::quantize_coordinate(event.y_norm));
    detail::write_be<std::uint16_t>(bytes, 14, static_cast<std::uint16_t>(event.wheel_delta));
    detail::write_be<std::uint32_t>(bytes, 16, event.key_code);
    bytes[20] = static_cast<std::uint8_t>(event.text.size());
    std::copy(event.text.begin(), event.text.end(), out.begin() + static_cast<std::ptrdiff_t>(kInputEventHeaderBytes));
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}

inline Result<InputEvent> deserialize_input_event(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kInputEventHeaderBytes)
        return Result<InputEvent>::failure(detail::protocol_error("en-tête d'entrée tronqué"));
    if (detail::read_be<std::uint32_t>(bytes, 0) != detail::kInputMagic)
        return Result<InputEvent>::failure(detail::protocol_error("magic d'entrée invalide"));
    if (bytes[4] != 1)
        return Result<InputEvent>::failure({ErrorCode::UnsupportedVersion, "version d'entrée non prise en charge", "media"});
    if (bytes[7] != 0 || bytes[21] != 0 || bytes[22] != 0 || bytes[23] != 0)
        return Result<InputEvent>::failure(detail::protocol_error("octets réservés d'entrée non nuls"));
    const std::size_t text_size = bytes[20];
    if (text_size > kMaxInputTextBytes || bytes.size() != kInputEventHeaderBytes + text_size)
        return Result<InputEvent>::failure(detail::protocol_error("longueur texte entrée invalide"));

    InputEvent event;
    event.type = static_cast<InputEventType>(bytes[5]);
    event.button = bytes[6];
    event.modifiers = detail::read_be<std::uint16_t>(bytes, 8);
    event.x_norm = static_cast<float>(detail::read_be<std::uint16_t>(bytes, 10)) / 65535.0f;
    event.y_norm = static_cast<float>(detail::read_be<std::uint16_t>(bytes, 12)) / 65535.0f;
    event.wheel_delta = static_cast<std::int16_t>(detail::read_be<std::uint16_t>(bytes, 14));
    event.key_code = detail::read_be<std::uint32_t>(bytes, 16);
    event.text.assign(reinterpret_cast<const char*>(bytes.data() + kInputEventHeaderBytes), text_size);
    auto valid = detail::validate_input(event);
    if (!valid) return Result<InputEvent>::failure(detail::protocol_error(valid.error().message));
    return Result<InputEvent>::success(std::move(event));
}

} // namespace plpt::media
