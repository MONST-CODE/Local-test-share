#pragma once
// DevLink — engine/browser/remote_frame.hpp
// Définition des structures de trames vidéo distantes et des évènements d'interaction.
// Mandat Trajectoire 2 (§15 Capture, encodage et décodage, §16 Retour des interactions).
#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <cstring>
#include "../core/types.hpp"
#include "../core/result.hpp"

namespace plpt::media {

enum class FrameFormat : std::uint8_t {
    RawRGBA32 = 0,
    RunLengthRGBA32 = 1,
    CompressedDelta = 2
};

struct FrameHeader {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t frame_index = 0;
    std::uint64_t timestamp_ms = 0;
    FrameFormat format = FrameFormat::RawRGBA32;
    std::uint8_t flags = 0; // bit 0 = keyframe
    std::uint16_t reserved = 0;
};
inline constexpr std::size_t kFrameHeaderBytes = 20;

struct VideoFrame {
    FrameHeader header{};
    std::vector<std::uint8_t> pixels; // Données pixels RGBA ou compressées
};

// Types d'évènements d'interaction invité -> hôte (§16)
enum class InputEventType : std::uint8_t {
    MouseMove = 1,
    MouseDown = 2,
    MouseUp   = 3,
    MouseWheel = 4,
    KeyDown   = 5,
    KeyUp     = 6,
    TextInput = 7
};

struct InputEvent {
    InputEventType type = InputEventType::MouseMove;
    float x_norm = 0.0f; // Coordonnée X normalisée [0.0 .. 1.0]
    float y_norm = 0.0f; // Coordonnée Y normalisée [0.0 .. 1.0]
    std::uint8_t button = 0; // 1=Gauche, 2=Droit, 3=Milieu
    std::uint16_t modifiers = 0; // bit 0=Shift, bit 1=Ctrl, bit 2=Alt
    std::int16_t wheel_delta = 0;
    std::uint32_t key_code = 0;
    char text[8]{}; // Caractère UTF-8 pour TextInput
};
inline constexpr std::size_t kInputEventBytes = 24;

// Encodage d'un évènement d'interaction en octets
inline std::vector<std::uint8_t> serialize_input_event(const InputEvent& ev) {
    std::vector<std::uint8_t> buf(kInputEventBytes, 0);
    buf[0] = static_cast<std::uint8_t>(ev.type);
    buf[1] = ev.button;
    std::memcpy(&buf[2], &ev.modifiers, 2);
    std::memcpy(&buf[4], &ev.x_norm, 4);
    std::memcpy(&buf[8], &ev.y_norm, 4);
    std::memcpy(&buf[12], &ev.wheel_delta, 2);
    std::memcpy(&buf[14], &ev.key_code, 4);
    std::memcpy(&buf[18], ev.text, 6);
    return buf;
}

// Décodage d'un évènement d'interaction reçu
inline Result<InputEvent> deserialize_input_event(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kInputEventBytes) {
        return Result<InputEvent>::failure(
            Error{ErrorCode::ProtocolViolation, "trame d'entrée trop courte", "media"});
    }
    InputEvent ev;
    ev.type = static_cast<InputEventType>(bytes[0]);
    ev.button = bytes[1];
    std::memcpy(&ev.modifiers, &bytes[2], 2);
    std::memcpy(&ev.x_norm, &bytes[4], 4);
    std::memcpy(&ev.y_norm, &bytes[8], 4);
    std::memcpy(&ev.wheel_delta, &bytes[12], 2);
    std::memcpy(&ev.key_code, &bytes[14], 4);
    std::memcpy(ev.text, &bytes[18], 6);
    ev.text[6] = '\0';
    return Result<InputEvent>::success(ev);
}

// Encodage d'une trame vidéo
inline std::vector<std::uint8_t> serialize_video_frame(const VideoFrame& frame) {
    std::vector<std::uint8_t> out(kFrameHeaderBytes + frame.pixels.size());
    std::memcpy(&out[0], &frame.header.width, 2);
    std::memcpy(&out[2], &frame.header.height, 2);
    std::memcpy(&out[4], &frame.header.frame_index, 4);
    std::memcpy(&out[8], &frame.header.timestamp_ms, 8);
    out[16] = static_cast<std::uint8_t>(frame.header.format);
    out[17] = frame.header.flags;
    std::memcpy(&out[18], &frame.header.reserved, 2);
    if (!frame.pixels.empty()) {
        std::memcpy(&out[kFrameHeaderBytes], frame.pixels.data(), frame.pixels.size());
    }
    return out;
}

// Décodage d'une trame vidéo reçue
inline Result<VideoFrame> deserialize_video_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kFrameHeaderBytes) {
        return Result<VideoFrame>::failure(
            Error{ErrorCode::ProtocolViolation, "trame vidéo trop courte", "media"});
    }
    VideoFrame frame;
    std::memcpy(&frame.header.width, &bytes[0], 2);
    std::memcpy(&frame.header.height, &bytes[2], 2);
    std::memcpy(&frame.header.frame_index, &bytes[4], 4);
    std::memcpy(&frame.header.timestamp_ms, &bytes[8], 8);
    frame.header.format = static_cast<FrameFormat>(bytes[16]);
    frame.header.flags = bytes[17];
    std::memcpy(&frame.header.reserved, &bytes[18], 2);

    const std::size_t payload_len = bytes.size() - kFrameHeaderBytes;
    frame.pixels.resize(payload_len);
    if (payload_len > 0) {
        std::memcpy(frame.pixels.data(), &bytes[kFrameHeaderBytes], payload_len);
    }
    return Result<VideoFrame>::success(std::move(frame));
}

} // namespace plpt::media
