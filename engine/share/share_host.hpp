#pragma once
// DevLink — engine/share/share_host.hpp — host side of the real sharing
// pipeline (ADR-018):
//
//   Browser Worker (real engine) -> compositor JPEG frames -> per-guest TLS 1.3
//   media channel -> guest; guest input records -> SharePolicy -> browser.
//
// Lifecycle (transactional): Validating -> StartingBrowser -> Navigating ->
// AwaitingFirstFrame -> OpeningMediaChannel -> Active. `Active` is published
// only after a REAL first frame was produced by the browser and the media
// listener is open. Any failure rolls back every completed step. stop() is
// idempotent and leaves no browser process, socket or thread behind.
//
// Threads: acceptor (accept + bounded handshakes), one I/O thread per guest
// media channel (owns its TLS object exclusively), one context worker (browser
// creation/destruction, which may take seconds). HostServer callbacks never
// block: they only issue/invalidate grants and enqueue work.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../browser/browser.hpp"
#include "../browser/egress_guard.hpp"
#include "../core/result.hpp"
#include "../net/host_server.hpp"
#include "../platform/socket.hpp"
#include "share_policy.hpp"

namespace plpt::share {

enum class ShareStage : std::uint8_t {
    Idle, Validating, StartingBrowser, Navigating, AwaitingFirstFrame, OpeningMediaChannel, Active, Stopping, Stopped, Failed
};
const char* share_stage_text(ShareStage stage);

struct ShareHostConfig {
    ShareMode mode = ShareMode::Collaborative;
    std::vector<std::string> allowed_origins; // explicit host selection, canonical
    std::string entry_url;                    // must belong to allowed_origins
    IpAddress bind_address = IpAddress::loopback(IpFamily::Ipv4); // same interface as the control plane
    std::uint32_t viewport_width = 1280;
    std::uint32_t viewport_height = 720;
    std::size_t max_guests = 5;
    std::uint32_t max_fps = 30;
    std::chrono::milliseconds first_frame_timeout{15000};
    std::uint32_t tls_handshake_timeout_ms = 5000;
};

struct SharePeerSnapshot {
    ParticipantId id{};
    std::string name;
    bool media_connected = false;
    bool context_ready = false;
    bool may_input = false;
    ContextId context{};
    std::uint64_t frames_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t inputs_accepted = 0;
    std::uint64_t inputs_rejected = 0;
    std::string last_rejection;
    double send_fps = 0.0;
};

struct ShareSnapshot {
    ShareStage stage = ShareStage::Idle;
    std::string error;
    ShareMode mode = ShareMode::Collaborative;
    std::uint16_t media_port = 0;
    std::string entry_url;
    std::vector<std::string> origins;
    ParticipantId controller{};
    std::vector<ParticipantId> waiting;
    bool paused = false;
    std::vector<SharePeerSnapshot> peers;
    BrowserContextStats primary;
    std::uint64_t handshakes_ok = 0;
    std::uint64_t handshakes_failed = 0;
    std::uint64_t first_frame_ms = 0;      // start() -> first real frame
    std::uint64_t contexts_created = 0;
    std::uint64_t contexts_destroyed = 0;
};

class ShareHost final : public net::IShareObserver {
public:
    // Takes ownership of the browser backend; it must be a real engine.
    explicit ShareHost(std::unique_ptr<IRemoteBrowserBackend> backend);
    ~ShareHost() override;
    ShareHost(const ShareHost&) = delete;
    ShareHost& operator=(const ShareHost&) = delete;

    Result<void> start(ShareHostConfig config);
    void stop();
    ShareSnapshot snapshot() const;
    bool active() const;

    // Host commands (thread-safe).
    bool grant_control(ParticipantId participant); // 0 = take control back from everyone
    void set_paused(bool paused);

    // net::IShareObserver — called on the HostServer I/O thread, never blocks.
    std::optional<net::MediaOffer> on_participant_admitted(SessionId session, ParticipantId participant,
        const std::string& name) override;
    void on_participant_removed(ParticipantId participant, bool revoked) override;

    // Host-side diagnostics only (qualification): reads the DOM of the context
    // a participant drives (0 = primary/shared context). Never guest-reachable.
    Result<std::string> evaluate_for_diagnostics(ParticipantId participant, const std::string& script);
    ContextId context_of(ParticipantId participant) const;

    // Host-side preview of the shared (primary) context: newest compositor
    // frame with sequence > after, or nullptr. Never blocks.
    std::shared_ptr<const CapturedFrame> preview(std::uint64_t after) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace plpt::share
