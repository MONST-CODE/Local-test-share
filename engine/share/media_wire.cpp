// DevLink — engine/share/media_wire.cpp
#include "media_wire.hpp"

#include <algorithm>

#include "../net/wire.hpp"

namespace plpt::share {

namespace {
Error violation(const char* what) { return {ErrorCode::ProtocolViolation, what, "media-wire"}; }

std::vector<std::uint8_t> with_length(std::vector<std::uint8_t> body) {
    std::vector<std::uint8_t> out(4 + body.size());
    const auto n = static_cast<std::uint32_t>(body.size());
    out[0] = static_cast<std::uint8_t>(n >> 24);
    out[1] = static_cast<std::uint8_t>(n >> 16);
    out[2] = static_cast<std::uint8_t>(n >> 8);
    out[3] = static_cast<std::uint8_t>(n);
    std::copy(body.begin(), body.end(), out.begin() + 4);
    return out;
}

Result<void> expect_kind(std::span<const std::uint8_t> body, RecordKind kind) {
    if (body.empty() || body[0] != static_cast<std::uint8_t>(kind))
        return Result<void>::failure(violation("type d'enregistrement inattendu"));
    return Result<void>::success();
}
} // namespace

std::vector<std::uint8_t> encode_hello(const MediaHello& h) {
    net::PayloadWriter w;
    w.u8(static_cast<std::uint8_t>(RecordKind::Hello));
    w.u32(kMediaHelloMagic);
    w.u16(kMediaChannelVersion);
    w.u64(h.session);
    w.u64(h.participant);
    return with_length(w.take());
}

Result<MediaHello> decode_hello(std::span<const std::uint8_t> body) {
    if (auto k = expect_kind(body, RecordKind::Hello); !k) return Result<MediaHello>::failure(k.error());
    net::PayloadReader r(body.subspan(1));
    auto magic = r.u32();
    auto version = r.u16();
    auto session = r.u64();
    auto participant = r.u64();
    if (!magic || !version || !session || !participant || !r.require_end())
        return Result<MediaHello>::failure(violation("hello média tronqué"));
    if (magic.value() != kMediaHelloMagic) return Result<MediaHello>::failure(violation("magic média invalide"));
    if (version.value() != kMediaChannelVersion)
        return Result<MediaHello>::failure({ErrorCode::UnsupportedVersion, "version de canal média", "media-wire"});
    if (session.value() == 0 || participant.value() == 0)
        return Result<MediaHello>::failure(violation("identifiants nuls"));
    return Result<MediaHello>::success(MediaHello{session.value(), participant.value()});
}

Result<std::vector<std::uint8_t>> encode_frame(const FrameRecord& f) {
    auto frame = media::serialize_video_frame(f.frame);
    if (!frame) return Result<std::vector<std::uint8_t>>::failure(frame.error());
    std::vector<std::uint8_t> body;
    body.reserve(9 + frame.value().size());
    body.push_back(static_cast<std::uint8_t>(RecordKind::Frame));
    for (int i = 7; i >= 0; --i) body.push_back(static_cast<std::uint8_t>(f.last_input_seq >> (i * 8)));
    body.insert(body.end(), frame.value().begin(), frame.value().end());
    return Result<std::vector<std::uint8_t>>::success(with_length(std::move(body)));
}

Result<FrameRecord> decode_frame(std::span<const std::uint8_t> body) {
    if (auto k = expect_kind(body, RecordKind::Frame); !k) return Result<FrameRecord>::failure(k.error());
    if (body.size() < 9) return Result<FrameRecord>::failure(violation("frame tronquée"));
    FrameRecord f;
    for (int i = 1; i <= 8; ++i) f.last_input_seq = (f.last_input_seq << 8) | body[static_cast<std::size_t>(i)];
    auto frame = media::deserialize_video_frame(body.subspan(9));
    if (!frame) return Result<FrameRecord>::failure(frame.error());
    f.frame = std::move(frame).value();
    return Result<FrameRecord>::success(std::move(f));
}

Result<std::vector<std::uint8_t>> encode_input(const InputRecord& in) {
    if (in.input_seq == 0) return Result<std::vector<std::uint8_t>>::failure({ErrorCode::InvalidArgument, "séquence nulle", "media-wire"});
    auto ev = media::serialize_input_event(in.event);
    if (!ev) return Result<std::vector<std::uint8_t>>::failure(ev.error());
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(RecordKind::Input));
    for (int i = 7; i >= 0; --i) body.push_back(static_cast<std::uint8_t>(in.input_seq >> (i * 8)));
    body.insert(body.end(), ev.value().begin(), ev.value().end());
    return Result<std::vector<std::uint8_t>>::success(with_length(std::move(body)));
}

Result<InputRecord> decode_input(std::span<const std::uint8_t> body) {
    if (auto k = expect_kind(body, RecordKind::Input); !k) return Result<InputRecord>::failure(k.error());
    if (body.size() < 9 || body.size() > 9 + media::kInputEventHeaderBytes + media::kMaxInputTextBytes)
        return Result<InputRecord>::failure(violation("entrée hors bornes"));
    InputRecord in;
    for (int i = 1; i <= 8; ++i) in.input_seq = (in.input_seq << 8) | body[static_cast<std::size_t>(i)];
    if (in.input_seq == 0) return Result<InputRecord>::failure(violation("séquence d'entrée nulle"));
    auto ev = media::deserialize_input_event(body.subspan(9));
    if (!ev) return Result<InputRecord>::failure(ev.error());
    in.event = std::move(ev).value();
    return Result<InputRecord>::success(std::move(in));
}

std::vector<std::uint8_t> encode_control(ControlAction action) {
    return with_length({static_cast<std::uint8_t>(RecordKind::Control), static_cast<std::uint8_t>(action)});
}

Result<ControlAction> decode_control(std::span<const std::uint8_t> body) {
    if (auto k = expect_kind(body, RecordKind::Control); !k) return Result<ControlAction>::failure(k.error());
    if (body.size() != 2 || (body[1] != 1 && body[1] != 2))
        return Result<ControlAction>::failure(violation("action de contrôle invalide"));
    return Result<ControlAction>::success(static_cast<ControlAction>(body[1]));
}

std::vector<std::uint8_t> encode_state(const StateRecord& s) {
    net::PayloadWriter w;
    w.u8(static_cast<std::uint8_t>(RecordKind::State));
    w.u8(static_cast<std::uint8_t>(s.mode));
    w.u64(s.you);
    w.u64(s.controller);
    w.u64(s.lease_epoch);
    std::uint8_t flags = 0;
    if (s.you_may_input) flags |= 1;
    if (s.you_waiting) flags |= 2;
    if (s.paused) flags |= 4;
    if (s.context_ready) flags |= 8;
    w.u8(flags);
    w.u32(s.viewport_width);
    w.u32(s.viewport_height);
    w.u32(s.last_rejection);
    w.text(s.note.substr(0, 200));
    return with_length(w.take());
}

Result<StateRecord> decode_state(std::span<const std::uint8_t> body) {
    if (auto k = expect_kind(body, RecordKind::State); !k) return Result<StateRecord>::failure(k.error());
    net::PayloadReader r(body.subspan(1));
    auto mode = r.u8();
    auto you = r.u64();
    auto controller = r.u64();
    auto epoch = r.u64();
    auto flags = r.u8();
    auto w = r.u32();
    auto h = r.u32();
    auto rejection = r.u32();
    auto note = r.text(200);
    if (!mode || !you || !controller || !epoch || !flags || !w || !h || !rejection || !note || !r.require_end())
        return Result<StateRecord>::failure(violation("état média tronqué"));
    auto m = share_mode_from_byte(mode.value());
    if (!m || (flags.value() & 0xF0) || w.value() > media::kMaxFrameWidth || h.value() > media::kMaxFrameHeight)
        return Result<StateRecord>::failure(violation("état média invalide"));
    StateRecord s;
    s.mode = *m;
    s.you = you.value();
    s.controller = controller.value();
    s.lease_epoch = epoch.value();
    s.you_may_input = flags.value() & 1;
    s.you_waiting = flags.value() & 2;
    s.paused = flags.value() & 4;
    s.context_ready = flags.value() & 8;
    s.viewport_width = w.value();
    s.viewport_height = h.value();
    s.last_rejection = rejection.value();
    s.note = std::move(note).value();
    return Result<StateRecord>::success(std::move(s));
}

std::vector<std::uint8_t> encode_closing(const ClosingRecord& c) {
    net::PayloadWriter w;
    w.u8(static_cast<std::uint8_t>(RecordKind::Closing));
    w.u16(static_cast<std::uint16_t>(c.reason));
    w.text(c.text.substr(0, 200));
    return with_length(w.take());
}

Result<ClosingRecord> decode_closing(std::span<const std::uint8_t> body) {
    if (auto k = expect_kind(body, RecordKind::Closing); !k) return Result<ClosingRecord>::failure(k.error());
    net::PayloadReader r(body.subspan(1));
    auto reason = r.u16();
    auto text = r.text(200);
    if (!reason || !text || !r.require_end() || reason.value() > static_cast<std::uint16_t>(ErrorCode::TooLarge))
        return Result<ClosingRecord>::failure(violation("fermeture média invalide"));
    return Result<ClosingRecord>::success(ClosingRecord{static_cast<ErrorCode>(reason.value()), std::move(text).value()});
}

Result<void> RecordAssembler::feed(std::span<const std::uint8_t> bytes) {
    if (failed_) return Result<void>::failure({ErrorCode::ProtocolViolation, "flux média déjà invalide", "media-wire"});
    if (offset_ > 0 && offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > (1u << 20)) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    if (buffered() >= 4) {
        const auto* p = buffer_.data() + offset_;
        const std::size_t n = (std::size_t{p[0]} << 24) | (std::size_t{p[1]} << 16) | (std::size_t{p[2]} << 8) | p[3];
        if (n == 0 || n > max_record_) {
            failed_ = true;
            return Result<void>::failure({ErrorCode::TooLarge, "enregistrement média hors bornes", "media-wire"});
        }
    }
    return Result<void>::success();
}

std::optional<std::vector<std::uint8_t>> RecordAssembler::next() {
    if (buffered() < 4) return std::nullopt;
    const auto* p = buffer_.data() + offset_;
    const std::size_t n = (std::size_t{p[0]} << 24) | (std::size_t{p[1]} << 16) | (std::size_t{p[2]} << 8) | p[3];
    if (n == 0 || n > max_record_) {
        failed_ = true;
        return std::nullopt;
    }
    if (buffered() < 4 + n) return std::nullopt;
    std::vector<std::uint8_t> body(buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + 4),
        buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + 4 + n));
    offset_ += 4 + n;
    return body;
}

} // namespace plpt::share
