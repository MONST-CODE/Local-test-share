#pragma once
// DevLink — engine/share/share_guest.hpp — guest side of the media channel.
//
// Connects to the host's media port with the grant received over the encrypted
// control channel, sends the clear Hello, completes TLS 1.3 PSK with the
// participant key, then on ONE I/O thread: receives frame records, validates
// and decodes them (OS JPEG codec) and publishes the newest decoded image;
// sends queued input records (mouse moves coalesced) and control requests.
// The UI thread only reads snapshots and the latest image: never blocks.
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "../core/result.hpp"
#include "../media/jpeg_decoder.hpp"
#include "../net/wire.hpp"
#include "../platform/socket.hpp"
#include "media_wire.hpp"

namespace plpt::share {

enum class MediaPhase : std::uint8_t { Idle, Connecting, Handshaking, Streaming, Closed, Failed };
const char* media_phase_text(MediaPhase phase);

struct ShareGuestConfig {
    IpAddress host;                 // address that reached the control plane
    net::MediaOffer offer;          // contains the PSK: never logged
    SessionId session{};
    ParticipantId self{};
    std::uint32_t connect_timeout_ms = 4000;
    std::uint32_t tls_handshake_timeout_ms = 5000;
};

struct GuestImage {
    std::uint64_t sequence = 0;     // host frame sequence
    std::uint64_t received_ns = 0;  // local steady clock
    media::DecodedImage image;
};

struct MediaGuestSnapshot {
    MediaPhase phase = MediaPhase::Idle;
    std::string status;             // displayable, never a secret
    std::string transport;          // "TLSv1.3 TLS_AES_128_GCM_SHA256" once established
    StateRecord state;              // last share state from the host
    bool have_state = false;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t invalid_records = 0;
    std::uint64_t decode_failures = 0;
    double fps = 0.0;
    double kbps = 0.0;
    double decode_ms_avg = 0.0;
    double input_rtt_ms_last = 0.0;  // input sent -> first frame that reflects it
    double input_rtt_ms_avg = 0.0;
    std::uint64_t inputs_sent = 0;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
};

class ShareGuest {
public:
    ShareGuest();
    ~ShareGuest();
    ShareGuest(const ShareGuest&) = delete;
    ShareGuest& operator=(const ShareGuest&) = delete;

    Result<void> start(ShareGuestConfig config);
    void stop(); // idempotent
    MediaGuestSnapshot snapshot() const;
    // Newest decoded image with sequence > after, or nullptr.
    std::shared_ptr<const GuestImage> latest(std::uint64_t after = 0) const;
    // Thread-safe, bounded; returns false when not streaming or queue full.
    bool send_input(const media::InputEvent& event);
    bool request_control();
    bool release_control();
    // TEST/QUALIFICATION ONLY: sends a raw, possibly hostile, record body as-is
    // (length prefix added) to prove the host refuses it server-side.
    bool send_raw_for_test(std::vector<std::uint8_t> record_with_length);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace plpt::share
