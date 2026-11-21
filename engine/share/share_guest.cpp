// DevLink — engine/share/share_guest.cpp — see share_guest.hpp.
#include "share_guest.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

#include "../net/transport.hpp"

namespace plpt::share {

using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

const char* media_phase_text(MediaPhase phase) {
    switch (phase) {
    case MediaPhase::Idle: return "inactif";
    case MediaPhase::Connecting: return "connexion au canal média";
    case MediaPhase::Handshaking: return "authentification TLS 1.3";
    case MediaPhase::Streaming: return "flux reçu";
    case MediaPhase::Closed: return "fermé";
    case MediaPhase::Failed: return "échec";
    }
    return "inconnu";
}

namespace {
constexpr std::size_t kMaxQueuedInputs = 512;
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}
} // namespace

struct ShareGuest::Impl {
    ShareGuestConfig config;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> streaming{false};

    mutable std::mutex mutex;
    MediaGuestSnapshot snap;
    std::shared_ptr<const GuestImage> image;
    std::deque<std::vector<std::uint8_t>> outbox; // complete records (with length)
    std::deque<media::InputEvent> inputs;
    std::uint64_t next_input_seq = 1;
    std::map<std::uint64_t, std::uint64_t> input_sent_ns; // seq -> send time (bounded)
    std::uint64_t rtt_samples = 0;

    void set_phase(MediaPhase p, std::string status) {
        std::lock_guard l(mutex);
        snap.phase = p;
        snap.status = std::move(status);
    }

    void run() {
        set_phase(MediaPhase::Connecting, "connexion au canal média de l'hôte");
        auto s = Socket::create(TransportProtocol::Tcp, config.host.family);
        if (!s) { set_phase(MediaPhase::Failed, "socket impossible"); return; }
        Socket sock = std::move(s).value();
        if (auto c = connect_with_timeout(sock, config.host, Port{config.offer.port},
                std::chrono::milliseconds(config.connect_timeout_ms)); !c) {
            set_phase(MediaPhase::Failed, "hôte injoignable sur le port média (pare-feu ?) : " + c.error().message);
            return;
        }
        (void)sock.set_nonblocking(false);
        (void)sock.set_no_delay(true);
        const auto hello = encode_hello(MediaHello{config.session.value, config.self.value});
        std::size_t sent = 0;
        (void)sock.set_timeouts(3000ms, 3000ms);
        while (sent < hello.size()) {
            auto w = sock.send(std::span<const std::uint8_t>(hello.data() + sent, hello.size() - sent));
            if (!w || w.value() == 0) { set_phase(MediaPhase::Failed, "envoi du hello média impossible"); return; }
            sent += w.value();
        }
        set_phase(MediaPhase::Handshaking, "authentification TLS 1.3 du canal média");
        auto tls = net::make_tls13_factory();
        if (!tls || !tls.value()->available()) {
            set_phase(MediaPhase::Failed, "TLS 1.3 indisponible sur cet invité : canal média refusé (jamais en clair)");
            return;
        }
        const auto fd = static_cast<net::TransportDescriptor>(sock.release());
        auto channel = tls.value()->wrap_client(fd, net::TransportCredentials{config.offer.identity, config.offer.key},
            config.tls_handshake_timeout_ms);
        if (!channel) {
            set_phase(MediaPhase::Failed, "authentification du canal média refusée : " + channel.error().message);
            return;
        }
        auto info = channel.value()->info();
        if (info.kind != net::TransportKind::Tls13Psk || !info.peer_authenticated || info.protocol != "TLSv1.3") {
            set_phase(MediaPhase::Failed, "canal média non conforme (TLS 1.3 PSK authentifié exigé)");
            return;
        }
        {
            std::lock_guard l(mutex);
            snap.transport = info.protocol + " " + info.cipher;
            snap.phase = MediaPhase::Streaming;
            snap.status = "connecté, en attente de la première image";
        }
        streaming.store(true);
        loop(*channel.value());
        streaming.store(false);
        channel.value()->close();
        std::lock_guard l(mutex);
        if (snap.phase == MediaPhase::Streaming) {
            snap.phase = MediaPhase::Closed;
            if (snap.status.empty() || snap.status.rfind("connecté", 0) == 0) snap.status = "canal média fermé";
        }
    }

    void on_frame(const FrameRecord& record, media::JpegDecoder& decoder, std::size_t bytes) {
        const auto& h = record.frame.header;
        if (h.session.value != config.session.value || h.participant != config.self || h.codec != media::FrameCodec::Jpeg) {
            std::lock_guard l(mutex);
            snap.invalid_records++;
            return;
        }
        auto img = std::make_shared<GuestImage>();
        img->sequence = h.sequence;
        img->received_ns = now_ns();
        const auto t0 = SteadyClock::now();
        auto decoded = decoder.decode(record.frame.pixels, h.width, h.height, img->image);
        const double decode_ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
        std::lock_guard l(mutex);
        if (!decoded) {
            snap.decode_failures++;
            return;
        }
        snap.frames++;
        snap.bytes += bytes;
        snap.frame_width = h.width;
        snap.frame_height = h.height;
        snap.decode_ms_avg = snap.frames == 1 ? decode_ms : snap.decode_ms_avg * 0.9 + decode_ms * 0.1;
        snap.status = "flux réel reçu";
        // Input -> frame latency: first frame that reflects our input seq.
        // Only the newest reflected input is sampled: older entries are inputs
        // the host refused (or that were superseded) and would report a
        // meaningless delay if measured when a later input finally shows.
        if (record.last_input_seq != 0) {
            auto newest = input_sent_ns.upper_bound(record.last_input_seq);
            if (newest != input_sent_ns.begin()) {
                const auto sampled = std::prev(newest);
                if (sampled->first == record.last_input_seq) {
                    const double rtt = static_cast<double>(img->received_ns - sampled->second) / 1e6;
                    snap.input_rtt_ms_last = rtt;
                    ++rtt_samples;
                    snap.input_rtt_ms_avg = rtt_samples == 1 ? rtt : snap.input_rtt_ms_avg * 0.9 + rtt * 0.1;
                }
                input_sent_ns.erase(input_sent_ns.begin(), newest);
            }
        }
        if (!image || image->sequence < img->sequence) image = std::move(img);
    }

    void loop(net::ITransportChannel& ch) {
        media::JpegDecoder decoder;
        RecordAssembler assembler(kMaxMediaRecord);
        std::vector<std::uint8_t> pending;
        std::size_t offset = 0;
        std::vector<std::uint8_t> buf(256 * 1024);
        auto window = SteadyClock::now();
        std::uint64_t window_frames = 0, window_bytes = 0, last_frames = 0, last_bytes = 0;
        while (!stop.load()) {
            if (offset >= pending.size()) {
                pending.clear();
                offset = 0;
                std::lock_guard l(mutex);
                if (!outbox.empty()) {
                    pending = std::move(outbox.front());
                    outbox.pop_front();
                } else if (!inputs.empty()) {
                    InputRecord in;
                    in.event = std::move(inputs.front());
                    inputs.pop_front();
                    in.input_seq = next_input_seq++;
                    if (auto enc = encode_input(in)) {
                        pending = std::move(enc).value();
                        input_sent_ns[in.input_seq] = now_ns();
                        while (input_sent_ns.size() > 256) input_sent_ns.erase(input_sent_ns.begin());
                        snap.inputs_sent++;
                    }
                }
            }
            PollEntry e;
            e.socket = static_cast<NativeSocket>(ch.descriptor());
            e.want_read = true;
            e.want_write = offset < pending.size();
            auto r = poll_sockets(std::span<PollEntry>(&e, 1), e.want_write ? 10ms : 5ms);
            if (!r || e.failed) { set_phase(MediaPhase::Closed, "connexion média perdue"); return; }
            if (e.readable) {
                for (int i = 0; i < 64; ++i) {
                    auto got = ch.receive(std::span<std::uint8_t>(buf.data(), buf.size()));
                    if (!got) {
                        if (got.error().code == ErrorCode::WouldBlock) break;
                        set_phase(MediaPhase::Closed, "connexion média perdue (hôte arrêté ou réseau coupé)");
                        return;
                    }
                    if (got.value() == 0) { set_phase(MediaPhase::Closed, "l'hôte a fermé le canal média"); return; }
                    if (!assembler.feed(std::span<const std::uint8_t>(buf.data(), got.value()))) {
                        set_phase(MediaPhase::Failed, "flux média invalide (enregistrement hors bornes)");
                        return;
                    }
                    while (auto body = assembler.next()) {
                        if (body->empty()) continue;
                        switch (static_cast<RecordKind>((*body)[0])) {
                        case RecordKind::Frame: {
                            auto f = decode_frame(*body);
                            if (!f) { std::lock_guard l(mutex); snap.invalid_records++; break; }
                            on_frame(f.value(), decoder, body->size());
                            ++window_frames;
                            window_bytes += body->size();
                            break;
                        }
                        case RecordKind::State: {
                            auto s = decode_state(*body);
                            std::lock_guard l(mutex);
                            if (!s) { snap.invalid_records++; break; }
                            snap.state = std::move(s).value();
                            snap.have_state = true;
                            break;
                        }
                        case RecordKind::Closing: {
                            auto c = decode_closing(*body);
                            set_phase(MediaPhase::Closed, c ? c.value().text : std::string("fermeture par l'hôte"));
                            return;
                        }
                        default: {
                            std::lock_guard l(mutex);
                            snap.invalid_records++;
                        }
                        }
                    }
                    if (assembler.failed()) { set_phase(MediaPhase::Failed, "flux média invalide"); return; }
                }
            }
            if (offset < pending.size()) {
                auto w = ch.send(std::span<const std::uint8_t>(pending.data() + offset, pending.size() - offset));
                if (!w) {
                    if (w.error().code != ErrorCode::WouldBlock) { set_phase(MediaPhase::Closed, "envoi impossible"); return; }
                } else {
                    offset += w.value();
                }
            }
            const auto elapsed = SteadyClock::now() - window;
            if (elapsed >= 1s) {
                const double secs = std::chrono::duration<double>(elapsed).count();
                std::lock_guard l(mutex);
                snap.fps = static_cast<double>(window_frames - last_frames) / secs;
                snap.kbps = static_cast<double>(window_bytes - last_bytes) * 8.0 / 1000.0 / secs;
                last_frames = window_frames;
                last_bytes = window_bytes;
                window = SteadyClock::now();
            }
        }
    }
};

ShareGuest::ShareGuest() : impl_(std::make_unique<Impl>()) {}
ShareGuest::~ShareGuest() { stop(); }

Result<void> ShareGuest::start(ShareGuestConfig config) {
    if (impl_->thread.joinable()) return Result<void>::failure({ErrorCode::InvalidState, "canal média déjà démarré", "share-guest"});
    if (config.offer.port == 0 || config.offer.key.size() != 64 || config.self.value == 0 || config.session.value == 0 ||
        !config.host.valid())
        return Result<void>::failure({ErrorCode::InvalidArgument, "offre média incomplète", "share-guest"});
    if (!media::JpegDecoder::available())
        return Result<void>::failure({ErrorCode::Unsupported,
            "décodeur d'images indisponible sur cette plateforme invitée (Windows requis, ADR-018)", "share-guest"});
    impl_->config = std::move(config);
    impl_->stop.store(false);
    {
        std::lock_guard l(impl_->mutex);
        impl_->snap = MediaGuestSnapshot{};
        impl_->image.reset();
        impl_->outbox.clear();
        impl_->inputs.clear();
        impl_->next_input_seq = 1;
        impl_->input_sent_ns.clear();
    }
    impl_->thread = std::thread([this] { impl_->run(); });
    return Result<void>::success();
}

void ShareGuest::stop() {
    impl_->stop.store(true);
    if (impl_->thread.joinable()) impl_->thread.join();
    // Forget the key material as soon as the channel is gone.
    impl_->config.offer.key.assign(impl_->config.offer.key.size(), '0');
    impl_->config.offer.key.clear();
}

MediaGuestSnapshot ShareGuest::snapshot() const {
    std::lock_guard l(impl_->mutex);
    return impl_->snap;
}

std::shared_ptr<const GuestImage> ShareGuest::latest(std::uint64_t after) const {
    std::lock_guard l(impl_->mutex);
    if (!impl_->image || impl_->image->sequence <= after) return nullptr;
    return impl_->image;
}

bool ShareGuest::send_input(const media::InputEvent& event) {
    if (!impl_->streaming.load()) return false;
    if (!media::serialize_input_event(event)) return false; // invalid: never queued
    std::lock_guard l(impl_->mutex);
    auto& q = impl_->inputs;
    // Coalesce consecutive mouse moves: only the newest position matters.
    if (event.type == media::InputEventType::MouseMove && !q.empty() && q.back().type == media::InputEventType::MouseMove) {
        q.back() = event;
        return true;
    }
    if (q.size() >= kMaxQueuedInputs) return false;
    q.push_back(event);
    return true;
}

bool ShareGuest::request_control() {
    if (!impl_->streaming.load()) return false;
    std::lock_guard l(impl_->mutex);
    impl_->outbox.push_back(encode_control(ControlAction::RequestControl));
    return true;
}

bool ShareGuest::release_control() {
    if (!impl_->streaming.load()) return false;
    std::lock_guard l(impl_->mutex);
    impl_->outbox.push_back(encode_control(ControlAction::ReleaseControl));
    return true;
}

bool ShareGuest::send_raw_for_test(std::vector<std::uint8_t> record_with_length) {
    if (!impl_->streaming.load()) return false;
    std::lock_guard l(impl_->mutex);
    impl_->outbox.push_back(std::move(record_with_length));
    return true;
}

} // namespace plpt::share
