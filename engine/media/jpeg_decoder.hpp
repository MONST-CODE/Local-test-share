#pragma once
// DevLink — engine/media/jpeg_decoder.hpp — guest-side frame decoder (ADR-018).
// Windows: Windows Imaging Component (OS codec, patched by Windows Update, no
// redistributed library). Other platforms: explicit Unsupported until a
// maintained decoder is integrated and qualified.
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../core/result.hpp"

namespace plpt::media {

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // tightly packed RGBA8, width*height*4
};

// One decoder per thread (it owns COM state on Windows). Not thread-safe.
class JpegDecoder {
public:
    JpegDecoder();
    ~JpegDecoder();
    JpegDecoder(const JpegDecoder&) = delete;
    JpegDecoder& operator=(const JpegDecoder&) = delete;

    static bool available();
    // Decodes and verifies the result is exactly expected_width x expected_height.
    Result<void> decode(std::span<const std::uint8_t> jpeg, std::uint32_t expected_width,
        std::uint32_t expected_height, DecodedImage& out);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace plpt::media
