#pragma once
// DevLink — engine/share/view_mapping.hpp — pure geometry between the panel
// that shows a remote frame and the host browser viewport (ADR-018 §7).
//
// The guest draws the host frame letterboxed (aspect preserved, centred) inside
// whatever area the window gives it. Pointer positions are converted to
// normalised [0,1] coordinates of the FRAME, so the host maps them onto its own
// CSS viewport: guest zoom, panel size, letterboxing and DPI never shift a
// click. No I/O, no UI library: unit-tested directly.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace plpt::share {

struct ViewRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct NormalizedPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// Largest rectangle with the frame's aspect ratio inside `area`, centred.
// Degenerate inputs give an empty rectangle (nothing drawn, nothing mapped).
inline ViewRect fit_letterbox(const ViewRect& area, std::uint32_t frame_width, std::uint32_t frame_height) {
    if (frame_width == 0 || frame_height == 0 || !(area.width > 0.0f) || !(area.height > 0.0f)) return ViewRect{area.x, area.y, 0, 0};
    const float scale = std::min(area.width / static_cast<float>(frame_width), area.height / static_cast<float>(frame_height));
    ViewRect r;
    r.width = std::floor(static_cast<float>(frame_width) * scale);
    r.height = std::floor(static_cast<float>(frame_height) * scale);
    r.x = std::floor(area.x + (area.width - r.width) * 0.5f);
    r.y = std::floor(area.y + (area.height - r.height) * 0.5f);
    return r;
}

// Pointer (same coordinate space as `view`) -> normalised frame coordinates.
// Outside the picture (in the letterbox bars) -> nullopt, unless `clamp` is
// set (a drag that started inside keeps tracking at the nearest edge).
inline std::optional<NormalizedPoint> to_normalized(const ViewRect& view, float px, float py, bool clamp) {
    if (!(view.width > 0.0f) || !(view.height > 0.0f) || !std::isfinite(px) || !std::isfinite(py)) return std::nullopt;
    float nx = (px - view.x) / view.width;
    float ny = (py - view.y) / view.height;
    const bool inside = nx >= 0.0f && nx <= 1.0f && ny >= 0.0f && ny <= 1.0f;
    if (!inside && !clamp) return std::nullopt;
    nx = std::clamp(nx, 0.0f, 1.0f);
    ny = std::clamp(ny, 0.0f, 1.0f);
    return NormalizedPoint{nx, ny};
}

// Splits UTF-8 text into chunks of at most `max_bytes` that never cut a code
// point (input records carry at most 32 bytes of text). Invalid lead bytes are
// dropped rather than forwarded.
template <class Sink>
void split_utf8(const char* text, std::size_t max_bytes, Sink&& sink) {
    if (!text || max_bytes < 4) return;
    std::size_t start = 0, i = 0, chunk = 0;
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    std::size_t len = 0;
    while (s[len]) ++len;
    while (i < len) {
        std::size_t n = s[i] < 0x80 ? 1 : (s[i] >> 5) == 0x6 ? 2 : (s[i] >> 4) == 0xE ? 3 : (s[i] >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > len) {
            if (chunk) sink(text + start, chunk);
            ++i;
            start = i;
            chunk = 0;
            continue;
        }
        if (chunk + n > max_bytes) {
            sink(text + start, chunk);
            start = i;
            chunk = 0;
        }
        chunk += n;
        i += n;
    }
    if (chunk) sink(text + start, chunk);
}

} // namespace plpt::share
