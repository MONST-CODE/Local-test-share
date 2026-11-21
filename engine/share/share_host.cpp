// DevLink — engine/share/share_host.cpp — see share_host.hpp.
#include "share_host.hpp"

#include <algorithm>
#include <array>
#include <tuple>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <thread>

#include "../browser/remote_frame.hpp"
#include "../browser/url_origin.hpp"
#include "../core/digest.hpp"
#include "../core/random.hpp"
#include "../net/transport.hpp"
#include "media_wire.hpp"

namespace plpt::share {

using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

const char* share_stage_text(ShareStage stage) {
    switch (stage) {
    case ShareStage::Idle: return "inactif";
    case ShareStage::Validating: return "validation des origines";
    case ShareStage::StartingBrowser: return "démarrage du navigateur";
    case ShareStage::Navigating: return "chargement de l'application";
    case ShareStage::AwaitingFirstFrame: return "attente de la première image réelle";
    case ShareStage::OpeningMediaChannel: return "ouverture du canal média";
    case ShareStage::Active: return "partage actif";
    case ShareStage::Stopping: return "arrêt";
    case ShareStage::Stopped: return "arrêté";
    case ShareStage::Failed: return "échec";
    }
    return "inconnu";
}

namespace {
constexpr std::uint64_t kHostContextOwner = 0x7FFF'FFFF'FFFF'FFFFULL; // profile naming only
constexpr std::size_t kMaxPendingHandshakes = 8;
constexpr unsigned kMaxFailuresPerMinute = 10;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

Error share_error(ErrorCode code, std::string message) { return Error{code, std::move(message), "share"}; }

bool wait_io(std::intptr_t fd, bool write, std::chrono::milliseconds timeout) {
    PollEntry e;
    e.socket = static_cast<NativeSocket>(fd);
    e.want_read = !write;
    e.want_write = write;
    auto r = poll_sockets(std::span<PollEntry>(&e, 1), timeout);
    return r && r.value() > 0 && !e.failed && (write ? e.writable : e.readable);
}
} // namespace

struct ShareHost::Impl {
    struct Grant {
        std::string identity;
        std::string key;
        std::string name;
    };
    // Per-context record of injected input (participant, seq, time) so that a
    // frame can tell its viewer which of ITS inputs it already reflects.
    struct InputTrail {
        std::mutex mutex;
        std::deque<std::tuple<ParticipantId, std::uint64_t, std::uint64_t>> entries;
        void add(ParticipantId p, std::uint64_t seq, std::uint64_t ns) {
            std::lock_guard l(mutex);
            entries.emplace_back(p, seq, ns);
            if (entries.size() > 512) entries.pop_front();
        }
        std::uint64_t reflected(ParticipantId p, std::uint64_t captured_ns) {
            std::lock_guard l(mutex);
            std::uint64_t best = 0;
            for (const auto& [who, seq, ns] : entries)
                if (who == p && ns < captured_ns) best = std::max(best, seq);
            return best;
        }
    };
    struct Peer {
        ParticipantId id{};
        std::unique_ptr<net::ITransportChannel> channel;
        std::thread thread;
        std::atomic<bool> stop{false};
        std::atomic<bool> finished{false};
        std::atomic<std::uint64_t> frames_sent{0};
        std::atomic<std::uint64_t> bytes_sent{0};
        std::atomic<double> fps{0.0};
    };

    explicit Impl(std::unique_ptr<IRemoteBrowserBackend> b) : backend(std::move(b)) {}

    std::unique_ptr<IRemoteBrowserBackend> backend;
    ShareHostConfig config;
    std::unique_ptr<net::ITransportFactory> tls;

    mutable std::mutex mutex; // protects everything below unless atomic
    ShareStage stage = ShareStage::Idle;
    std::string error;
    std::unique_ptr<SharePolicy> policy;
    ContextId primary{};
    std::uint64_t session = 0;
    std::map<ParticipantId, Grant> grants;
    std::map<ParticipantId, std::shared_ptr<Peer>> peers;
    std::map<ContextId, std::shared_ptr<InputTrail>> trails;
    std::map<std::string, std::pair<std::uint64_t, unsigned>> failures; // ip -> (window start ms, count)
    std::size_t pending_handshakes = 0;
    std::uint64_t handshakes_ok = 0, handshakes_failed = 0;
    std::uint64_t first_frame_ms = 0;
    std::uint64_t contexts_created = 0, contexts_destroyed = 0;
    std::map<ParticipantId, std::uint32_t> last_rejection;

    Socket listener;
    std::uint16_t media_port = 0;
    std::atomic<bool> stopping{false};
    std::thread acceptor;
    struct HandshakeThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };
    std::list<HandshakeThread> handshake_threads;

    // context worker
    std::mutex work_mutex;
    std::condition_variable work_cv;
    std::deque<std::function<void()>> work;
    std::thread worker;
    bool worker_stop = false;

    void post_work(std::function<void()> task) {
        {
            std::lock_guard l(work_mutex);
            work.push_back(std::move(task));
        }
        work_cv.notify_one();
    }
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock l(work_mutex);
                work_cv.wait(l, [&] { return worker_stop || !work.empty(); });
                if (work.empty() && worker_stop) return;
                task = std::move(work.front());
                work.pop_front();
            }
            task();
        }
    }

    BrowserPolicy browser_policy() const {
        BrowserPolicy p;
        p.allowed_origins = config.allowed_origins;
        return p;
    }

    // ---------------- Mode A contexts (worker thread) ----------------
    void create_guest_context(ParticipantId p) {
        {
            std::lock_guard l(mutex);
            if (stopping.load() || !policy) return;
            const auto* existing = policy->find(p);
            if (!existing || existing->context.value != 0) return; // gone or already has one (resume)
        }
        auto ctx = backend->create_context(p, browser_policy());
        if (!ctx) {
            std::lock_guard l(mutex);
            last_rejection[p] = static_cast<std::uint32_t>(InputDecision::NoContext);
            return;
        }
        auto nav = backend->navigate(ctx.value().context, config.entry_url);
        std::lock_guard l(mutex);
        contexts_created++;
        const auto* still = policy ? policy->find(p) : nullptr;
        if (!nav || !still || stopping.load()) {
            post_work([this, c = ctx.value().context] { destroy_context(c); });
            return;
        }
        trails[ctx.value().context] = std::make_shared<InputTrail>();
        policy->set_context(p, ctx.value().context);
    }
    void destroy_context(ContextId c) {
        if (c.value == 0) return;
        (void)backend->destroy_context(c);
        std::lock_guard l(mutex);
        trails.erase(c);
        contexts_destroyed++;
    }

    // ---------------- Acceptor & handshakes ----------------
    void accept_loop() {
        while (!stopping.load()) {
            IpAddress peer;
            Port peer_port{};
            auto accepted = listener.accept(&peer, &peer_port);
            if (!accepted) {
                (void)wait_io(static_cast<std::intptr_t>(listener.native()), false, 100ms);
                reap();
                continue;
            }
            Socket s = std::move(accepted).value();
            std::lock_guard l(mutex);
            const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                SteadyClock::now().time_since_epoch()).count());
            auto& f = failures[peer.text];
            if (now_ms - f.first > 60000) f = {now_ms, 0};
            if (pending_handshakes >= kMaxPendingHandshakes || f.second >= kMaxFailuresPerMinute) {
                handshakes_failed++;
                s.close();
                continue;
            }
            pending_handshakes++;
            handshake_threads.emplace_back();
            auto done = handshake_threads.back().done;
            handshake_threads.back().thread = std::thread([this, done, sock = std::move(s), addr = peer.text]() mutable {
                handshake(std::move(sock), addr);
                done->store(true);
            });
        }
    }

    void count_failure(const std::string& addr) {
        std::lock_guard l(mutex);
        handshakes_failed++;
        failures[addr].second++;
    }

    void handshake(Socket sock, std::string addr) {
        struct Done {
            Impl* self;
            ~Done() { std::lock_guard l(self->mutex); self->pending_handshakes--; }
        } done{this};
        (void)sock.set_nonblocking(true);
        // 1. Clear hello: names the grant, proves nothing by itself.
        RecordAssembler assembler(kMaxHelloRecord);
        std::array<std::uint8_t, 128> buf{};
        std::optional<std::vector<std::uint8_t>> body;
        const auto deadline = SteadyClock::now() + 3s;
        while (!body && SteadyClock::now() < deadline && !stopping.load()) {
            if (!wait_io(static_cast<std::intptr_t>(sock.native()), false, 100ms)) continue;
            auto got = sock.receive(std::span<std::uint8_t>(buf.data(), buf.size()));
            if (!got) {
                if (got.error().code == ErrorCode::WouldBlock) continue;
                break;
            }
            if (got.value() == 0) break;
            // Only the hello may arrive before TLS: never read past it.
            if (!assembler.feed(std::span<const std::uint8_t>(buf.data(), got.value()))) break;
            body = assembler.next();
            if (body && assembler.buffered() != 0) body.reset(); // bytes after hello: refuse
            if (!body && assembler.buffered() > kMaxHelloRecord) break;
        }
        if (!body) { count_failure(addr); return; }
        auto hello = decode_hello(*body);
        Grant grant;
        {
            std::lock_guard l(mutex);
            auto it = hello ? grants.find(ParticipantId{hello.value().participant}) : grants.end();
            if (!hello || hello.value().session != session || it == grants.end() || stage != ShareStage::Active) {
                handshakes_failed++;
                failures[addr].second++;
                return;
            }
            grant = it->second;
        }
        // 2. TLS 1.3 PSK with THIS participant's key: a wrong key fails here.
        const auto fd = static_cast<net::TransportDescriptor>(sock.release());
        auto channel = tls->wrap_server(fd, net::TransportCredentials{grant.identity, grant.key},
            config.tls_handshake_timeout_ms);
        if (!channel) { count_failure(addr); return; }
        const auto info = channel.value()->info();
        if (info.kind != net::TransportKind::Tls13Psk || !info.peer_authenticated || info.protocol != "TLSv1.3") {
            count_failure(addr);
            return;
        }
        const ParticipantId pid{hello.value().participant};
        auto peer = std::make_shared<Peer>();
        peer->id = pid;
        peer->channel = std::move(channel).value();
        std::shared_ptr<Peer> replaced;
        {
            std::lock_guard l(mutex);
            // The grant may have been revoked during the handshake.
            if (!grants.count(pid) || stopping.load()) return;
            handshakes_ok++;
            auto it = peers.find(pid);
            if (it != peers.end()) {
                replaced = it->second;
                replaced->stop.store(true);
                retired.push_back(replaced); // joined by reap(), never leaked
            }
            peers[pid] = peer;
            peer->thread = std::thread([this, peer] { serve_peer(*peer); });
        }
    }

    void reap() {
        std::list<HandshakeThread> joinable;
        std::vector<std::shared_ptr<Peer>> finished;
        {
            std::lock_guard l(mutex);
            for (auto it = handshake_threads.begin(); it != handshake_threads.end();) {
                auto next = std::next(it);
                if (it->done->load()) joinable.splice(joinable.end(), handshake_threads, it);
                it = next;
            }
            for (auto it = peers.begin(); it != peers.end();) {
                if (it->second->finished.load()) {
                    finished.push_back(it->second);
                    it = peers.erase(it);
                } else {
                    ++it;
                }
            }
            retired.erase(std::remove_if(retired.begin(), retired.end(), [&](const std::shared_ptr<Peer>& p) {
                if (p->finished.load()) { finished.push_back(p); return true; }
                return false;
            }), retired.end());
        }
        for (auto& p : finished)
            if (p->thread.joinable()) p->thread.join();
        for (auto& h : joinable)
            if (h.thread.joinable()) h.thread.join();
    }
    std::vector<std::shared_ptr<Peer>> retired; // stopped peers not yet joined

    // ---------------- Per-guest media I/O ----------------
    StateRecord state_for(ParticipantId p) {
        StateRecord s;
        s.mode = policy->mode();
        s.you = p.value;
        s.controller = policy->lease().holder.value;
        s.lease_epoch = policy->lease().epoch;
        s.you_may_input = policy->may_input(p);
        const auto& w = policy->lease().waiting;
        s.you_waiting = std::find(w.begin(), w.end(), p) != w.end();
        s.paused = policy->paused();
        const auto* ps = policy->find(p);
        s.context_ready = ps && ps->context.value != 0;
        s.viewport_width = config.viewport_width;
        s.viewport_height = config.viewport_height;
        auto r = last_rejection.find(p);
        s.last_rejection = r == last_rejection.end() ? 0 : r->second;
        s.note = s.context_ready ? "" : "préparation du navigateur";
        return s;
    }

    void serve_peer(Peer& peer) {
        struct Finish {
            Peer& p;
            ~Finish() {
                if (p.channel) p.channel->close();
                p.finished.store(true);
            }
        } finish{peer};
        auto& ch = *peer.channel;
        RecordAssembler assembler(kMaxControlRecord);
        std::vector<std::uint8_t> pending;
        std::size_t offset = 0;
        std::uint64_t last_frame = 0;
        std::uint64_t sent_revision = 0;
        ContextId last_ctx{};
        auto last_frame_time = SteadyClock::now() - 1s;
        const auto min_interval = std::chrono::microseconds(1'000'000 / std::max<std::uint32_t>(1, config.max_fps));
        auto fps_window = SteadyClock::now();
        unsigned fps_frames = 0;
        std::array<std::uint8_t, 4096> buf{};
        while (!peer.stop.load() && !stopping.load()) {
            // ---- choose what to send next (never queue more than one record) ----
            if (offset >= pending.size()) {
                pending.clear();
                offset = 0;
                ContextId ctx{};
                {
                    std::lock_guard l(mutex);
                    if (!grants.count(peer.id) || !policy) break; // revoked or stopped
                    if (policy->revision() != sent_revision) {
                        sent_revision = policy->revision();
                        pending = encode_state(state_for(peer.id));
                    }
                    if (const auto* ps = policy->find(peer.id)) ctx = ps->context;
                }
                if (ctx != last_ctx) { last_ctx = ctx; last_frame = 0; }
                if (pending.empty() && ctx.value != 0 && SteadyClock::now() - last_frame_time >= min_interval) {
                    auto frame = backend->wait_frame(ctx, last_frame, 0ms);
                    if (frame) {
                        const auto& f = *frame.value();
                        FrameRecord record;
                        record.frame.header.codec = media::FrameCodec::Jpeg;
                        record.frame.header.session = SessionId{session};
                        record.frame.header.participant = peer.id;
                        record.frame.header.stream_id = 1;
                        record.frame.header.context = ctx;
                        record.frame.header.sequence = f.sequence;
                        record.frame.header.timestamp_ns = f.captured_ns;
                        record.frame.header.width = f.width;
                        record.frame.header.height = f.height;
                        record.frame.header.stride_bytes = 0;
                        record.frame.pixels = f.data;
                        std::shared_ptr<InputTrail> trail;
                        {
                            std::lock_guard l(mutex);
                            auto t = trails.find(ctx);
                            if (t != trails.end()) trail = t->second;
                        }
                        record.last_input_seq = trail ? trail->reflected(peer.id, f.captured_ns) : 0;
                        if (auto encoded = encode_frame(record)) {
                            pending = std::move(encoded).value();
                            last_frame = f.sequence;
                            last_frame_time = SteadyClock::now();
                            peer.frames_sent++;
                            ++fps_frames;
                        }
                    }
                }
            }
            if (SteadyClock::now() - fps_window >= 1s) {
                peer.fps.store(fps_frames / std::chrono::duration<double>(SteadyClock::now() - fps_window).count());
                fps_window = SteadyClock::now();
                fps_frames = 0;
            }
            // ---- wait for I/O ----
            PollEntry e;
            e.socket = static_cast<NativeSocket>(ch.descriptor());
            e.want_read = true;
            e.want_write = offset < pending.size();
            auto r = poll_sockets(std::span<PollEntry>(&e, 1), e.want_write ? 20ms : 4ms);
            if (!r || e.failed) break;
            if (e.readable) {
                bool closed = false;
                for (int i = 0; i < 64; ++i) {
                    auto got = ch.receive(std::span<std::uint8_t>(buf.data(), buf.size()));
                    if (!got) {
                        if (got.error().code != ErrorCode::WouldBlock) closed = true;
                        break;
                    }
                    if (got.value() == 0) { closed = true; break; }
                    if (!assembler.feed(std::span<const std::uint8_t>(buf.data(), got.value()))) { closed = true; break; }
                    while (auto record = assembler.next()) handle_record(peer, *record);
                    if (assembler.failed()) { closed = true; break; }
                }
                if (closed) break;
            }
            if (offset < pending.size()) {
                auto sent = ch.send(std::span<const std::uint8_t>(pending.data() + offset, pending.size() - offset));
                if (!sent) {
                    if (sent.error().code != ErrorCode::WouldBlock) break;
                } else {
                    offset += sent.value();
                    peer.bytes_sent += sent.value();
                }
            }
        }
        // Best effort: tell the guest why the stream ends (bounded, never blocks long).
        if (stopping.load() || peer.stop.load()) {
            auto bye = encode_closing(ClosingRecord{ErrorCode::Cancelled,
                stopping.load() ? "partage arrêté par l'hôte" : "accès média retiré"});
            std::size_t done = 0;
            const auto until = SteadyClock::now() + 200ms;
            while (done < bye.size() && SteadyClock::now() < until) {
                auto s = ch.send(std::span<const std::uint8_t>(bye.data() + done, bye.size() - done));
                if (s) done += s.value();
                else if (s.error().code != ErrorCode::WouldBlock) break;
                else (void)wait_io(ch.descriptor(), true, 20ms);
            }
        }
    }

    void handle_record(Peer& peer, const std::vector<std::uint8_t>& body) {
        if (body.empty()) return;
        const auto kind = static_cast<RecordKind>(body[0]);
        if (kind == RecordKind::Input) {
            auto in = decode_input(body);
            ContextId target{};
            InputDecision decision = InputDecision::UnknownParticipant;
            std::shared_ptr<InputTrail> trail;
            {
                std::lock_guard l(mutex);
                if (!policy || !grants.count(peer.id)) return; // revoked: nothing reaches the browser
                if (!in) {
                    last_rejection[peer.id] = static_cast<std::uint32_t>(InputDecision::Replayed);
                    return;
                }
                decision = policy->authorize(peer.id, in.value().input_seq, &target);
                if (decision != InputDecision::Accepted) {
                    last_rejection[peer.id] = static_cast<std::uint32_t>(decision);
                    return;
                }
                auto t = trails.find(target);
                if (t != trails.end()) trail = t->second;
            }
            auto bytes = media::serialize_input_event(in.value().event);
            if (!bytes) return;
            if (backend->inject_input(target, bytes.value()) && trail) trail->add(peer.id, in.value().input_seq, now_ns());
            return;
        }
        if (kind == RecordKind::Control) {
            auto action = decode_control(body);
            if (!action) return;
            std::lock_guard l(mutex);
            if (!policy || !grants.count(peer.id)) return;
            if (action.value() == ControlAction::RequestControl) (void)policy->request_control(peer.id);
            else policy->release_control(peer.id);
            return;
        }
        // Any other record from a guest is a protocol violation: ignored, counted.
        std::lock_guard l(mutex);
        last_rejection[peer.id] = static_cast<std::uint32_t>(InputDecision::UnknownParticipant);
    }

    // ---------------- Lifecycle ----------------
    void set_stage(ShareStage s) {
        std::lock_guard l(mutex);
        stage = s;
    }
    Result<void> fail(Error e) {
        rollback();
        std::lock_guard l(mutex);
        stage = ShareStage::Failed;
        error = e.message;
        return Result<void>::failure(std::move(e));
    }
    void rollback() {
        stopping.store(true);
        if (acceptor.joinable()) acceptor.join();
        std::vector<std::shared_ptr<Peer>> all;
        std::list<HandshakeThread> handshakes;
        {
            std::lock_guard l(mutex);
            for (auto& [id, p] : peers) all.push_back(p);
            peers.clear();
            for (auto& p : retired) all.push_back(p);
            retired.clear();
            grants.clear();
            handshakes.swap(handshake_threads);
        }
        for (auto& p : all) p->stop.store(true);
        for (auto& p : all)
            if (p->thread.joinable()) p->thread.join();
        for (auto& t : handshakes)
            if (t.thread.joinable()) t.thread.join();
        listener.close();
        media_port = 0;
        {
            std::lock_guard l(work_mutex);
            worker_stop = true;
        }
        work_cv.notify_all();
        if (worker.joinable()) worker.join();
        // Every context (primary and per-guest) is destroyed, then the engine
        // itself releases its browser process and temporary profile folder.
        std::vector<ContextId> contexts;
        {
            std::lock_guard l(mutex);
            if (primary.value) contexts.push_back(primary);
            if (policy)
                for (const auto& [id, p] : policy->participants())
                    if (p.context.value && p.context != primary) contexts.push_back(p.context);
            primary = ContextId{};
            policy.reset();
            trails.clear();
        }
        for (auto c : contexts) {
            (void)backend->destroy_context(c);
            std::lock_guard l(mutex);
            contexts_destroyed++;
        }
        backend->shutdown_engine();
    }
};

ShareHost::ShareHost(std::unique_ptr<IRemoteBrowserBackend> backend) : impl_(std::make_unique<Impl>(std::move(backend))) {}
ShareHost::~ShareHost() { stop(); }

Result<void> ShareHost::start(ShareHostConfig config) {
    auto& I = *impl_;
    {
        std::lock_guard l(I.mutex);
        if (I.stage != ShareStage::Idle && I.stage != ShareStage::Stopped && I.stage != ShareStage::Failed)
            return Result<void>::failure(share_error(ErrorCode::InvalidState, "partage déjà démarré"));
        I.stage = ShareStage::Validating;
        I.error.clear();
    }
    I.stopping.store(false);
    I.worker_stop = false;
    const auto t0 = SteadyClock::now();
    // 1. Explicit, canonical selection; entry URL inside it.
    if (config.allowed_origins.empty())
        return I.fail(share_error(ErrorCode::InvalidArgument, "aucune origine sélectionnée : la sélection explicite est obligatoire"));
    for (auto& o : config.allowed_origins) {
        auto canonical = browser::url_origin::canonicalize(o, true);
        if (!canonical) return I.fail(share_error(ErrorCode::InvalidArgument, "origine invalide : " + o));
        o = *canonical;
        if (!o.empty() && o.back() == '/') o.pop_back();
    }
    if (!browser::url_origin::is_allowed(config.entry_url, config.allowed_origins))
        return I.fail(share_error(ErrorCode::PermissionDenied, "l'URL d'entrée n'appartient pas aux origines sélectionnées"));
    if (config.max_guests == 0 || config.max_guests > 16)
        return I.fail(share_error(ErrorCode::InvalidArgument, "nombre d'invités hors bornes (1..16)"));
    if (!I.backend || !I.backend->real_engine())
        return I.fail(share_error(ErrorCode::Unsupported, "aucun moteur navigateur réel : partage refusé (jamais de démonstration silencieuse)"));
    auto tls = net::make_tls13_factory();
    if (!tls || !tls.value()->available())
        return I.fail(share_error(ErrorCode::Unsupported, "TLS 1.3 indisponible : le canal média ne sera jamais ouvert en clair"));
    I.tls = std::move(tls).value();
    I.config = config;
    {
        std::lock_guard l(I.mutex);
        I.policy = std::make_unique<SharePolicy>(config.mode);
    }
    // 2-3. Real browser context, real navigation.
    I.set_stage(ShareStage::StartingBrowser);
    auto ctx = I.backend->create_context(ParticipantId{kHostContextOwner}, I.browser_policy());
    if (!ctx) return I.fail(ctx.error());
    {
        std::lock_guard l(I.mutex);
        I.primary = ctx.value().context;
        I.contexts_created++;
        I.trails[I.primary] = std::make_shared<Impl::InputTrail>();
        I.policy->set_shared_context(I.primary);
    }
    I.set_stage(ShareStage::Navigating);
    if (auto nav = I.backend->navigate(I.primary, config.entry_url); !nav) return I.fail(nav.error());
    // 4. A REAL frame from the compositor, not a promise.
    I.set_stage(ShareStage::AwaitingFirstFrame);
    auto frame = I.backend->wait_frame(I.primary, 0, config.first_frame_timeout);
    if (!frame) return I.fail(share_error(ErrorCode::TimedOut, "aucune image réelle produite par le navigateur"));
    I.first_frame_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t0).count());
    // 5. Media listener on the control-plane interface, OS-assigned port, kept open.
    I.set_stage(ShareStage::OpeningMediaChannel);
    auto s = Socket::create(TransportProtocol::Tcp, config.bind_address.family);
    if (!s) return I.fail(s.error());
    I.listener = std::move(s).value();
    (void)I.listener.set_exclusive_bind(true);
    if (auto b = I.listener.bind(config.bind_address, Port{0}); !b) return I.fail(b.error());
    if (auto ls = I.listener.listen(16); !ls) return I.fail(ls.error());
    if (auto nb = I.listener.set_nonblocking(true); !nb) return I.fail(nb.error());
    auto port = I.listener.bound_port();
    if (!port) return I.fail(port.error());
    I.media_port = port.value().value;
    I.worker = std::thread([&I] { I.worker_loop(); });
    I.acceptor = std::thread([&I] { I.accept_loop(); });
    // 6. Active.
    I.set_stage(ShareStage::Active);
    return Result<void>::success();
}

void ShareHost::stop() {
    auto& I = *impl_;
    {
        std::lock_guard l(I.mutex);
        if (I.stage == ShareStage::Idle || I.stage == ShareStage::Stopped || I.stage == ShareStage::Failed) return;
        I.stage = ShareStage::Stopping;
    }
    I.rollback();
    std::lock_guard l(I.mutex);
    I.stage = ShareStage::Stopped;
}

bool ShareHost::active() const {
    std::lock_guard l(impl_->mutex);
    return impl_->stage == ShareStage::Active;
}

ShareSnapshot ShareHost::snapshot() const {
    auto& I = *impl_;
    ShareSnapshot s;
    ContextId primary{};
    {
        std::lock_guard l(I.mutex);
        s.stage = I.stage;
        s.error = I.error;
        s.mode = I.config.mode;
        s.media_port = I.media_port;
        s.entry_url = I.config.entry_url;
        s.origins = I.config.allowed_origins;
        s.handshakes_ok = I.handshakes_ok;
        s.handshakes_failed = I.handshakes_failed;
        s.first_frame_ms = I.first_frame_ms;
        s.contexts_created = I.contexts_created;
        s.contexts_destroyed = I.contexts_destroyed;
        primary = I.primary;
        if (I.policy) {
            s.mode = I.policy->mode();
            s.controller = I.policy->lease().holder;
            s.waiting.assign(I.policy->lease().waiting.begin(), I.policy->lease().waiting.end());
            s.paused = I.policy->paused();
            for (const auto& [id, p] : I.policy->participants()) {
                SharePeerSnapshot ps;
                ps.id = id;
                ps.name = p.name;
                ps.context = p.context;
                ps.context_ready = p.context.value != 0;
                ps.may_input = I.policy->may_input(id);
                ps.inputs_accepted = p.accepted;
                ps.inputs_rejected = p.rejected;
                auto r = I.last_rejection.find(id);
                if (r != I.last_rejection.end()) ps.last_rejection = input_decision_text(static_cast<InputDecision>(r->second));
                auto peer = I.peers.find(id);
                if (peer != I.peers.end()) {
                    ps.media_connected = !peer->second->finished.load();
                    ps.frames_sent = peer->second->frames_sent.load();
                    ps.bytes_sent = peer->second->bytes_sent.load();
                    ps.send_fps = peer->second->fps.load();
                }
                s.peers.push_back(std::move(ps));
            }
        }
    }
    if (primary.value) {
        if (auto st = I.backend->context_stats(primary)) s.primary = st.value();
    }
    return s;
}

bool ShareHost::grant_control(ParticipantId participant) {
    std::lock_guard l(impl_->mutex);
    return impl_->policy && impl_->policy->host_grant(participant);
}

void ShareHost::set_paused(bool paused) {
    std::lock_guard l(impl_->mutex);
    if (impl_->policy) impl_->policy->set_paused(paused);
}

std::optional<net::MediaOffer> ShareHost::on_participant_admitted(SessionId session, ParticipantId participant,
    const std::string& name) {
    auto& I = *impl_;
    std::lock_guard l(I.mutex);
    if (I.stage != ShareStage::Active || !I.policy) return std::nullopt;
    if (I.session == 0) I.session = session.value;
    if (I.session != session.value) return std::nullopt;
    if (!I.grants.count(participant) && I.grants.size() >= I.config.max_guests) return std::nullopt;
    if (!I.policy->add(participant, name)) return std::nullopt;
    std::array<std::uint8_t, 32> key{};
    SecureRandom random;
    if (!random.fill(key)) return std::nullopt;
    Impl::Grant g;
    g.identity = media_identity(session.value, participant.value);
    g.key = to_hex(key);
    g.name = name;
    // A new grant replaces any previous one: the old key stops working now.
    I.grants[participant] = g;
    if (auto old = I.peers.find(participant); old != I.peers.end()) {
        old->second->stop.store(true);
        I.retired.push_back(old->second);
        I.peers.erase(old);
    }
    if (I.config.mode == ShareMode::Independent) {
        const auto* p = I.policy->find(participant);
        if (p && p->context.value == 0) I.post_work([&I, participant] { I.create_guest_context(participant); });
    }
    net::MediaOffer offer;
    offer.port = I.media_port;
    offer.identity = g.identity;
    offer.key = g.key;
    offer.mode = static_cast<std::uint8_t>(I.config.mode);
    offer.viewport_width = I.config.viewport_width;
    offer.viewport_height = I.config.viewport_height;
    return offer;
}

void ShareHost::on_participant_removed(ParticipantId participant, bool revoked) {
    auto& I = *impl_;
    std::lock_guard l(I.mutex);
    // The key dies with the departure: nothing this participant sends is used.
    I.grants.erase(participant);
    if (auto it = I.peers.find(participant); it != I.peers.end()) {
        it->second->stop.store(true);
        I.retired.push_back(it->second);
        I.peers.erase(it);
    }
    if (!I.policy) return;
    ContextId own{};
    if (const auto* p = I.policy->find(participant); p && I.config.mode == ShareMode::Independent) own = p->context;
    I.policy->remove(participant, revoked);
    // Mode A: a revoked guest's browser context (cookies, storage) is destroyed;
    // a merely disconnected one keeps it for its resume grace period.
    if (revoked && own.value) I.post_work([&I, own] { I.destroy_context(own); });
}

Result<std::string> ShareHost::evaluate_for_diagnostics(ParticipantId participant, const std::string& script) {
    const ContextId c = context_of(participant);
    if (c.value == 0) return Result<std::string>::failure(share_error(ErrorCode::NotFound, "contexte introuvable"));
    return impl_->backend->evaluate_for_diagnostics(c, script);
}

ContextId ShareHost::context_of(ParticipantId participant) const {
    std::lock_guard l(impl_->mutex);
    if (participant.value == 0) return impl_->primary;
    if (!impl_->policy) return {};
    const auto* p = impl_->policy->find(participant);
    return p ? p->context : ContextId{};
}

std::shared_ptr<const CapturedFrame> ShareHost::preview(std::uint64_t after) const {
    const ContextId c = context_of(ParticipantId{});
    if (c.value == 0) return nullptr;
    auto frame = impl_->backend->wait_frame(c, after, std::chrono::milliseconds(0));
    return frame ? frame.value() : nullptr;
}

} // namespace plpt::share
