#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "engine/browser/remote_frame.hpp"
#include "engine/protocol/envelope.hpp"

namespace {
template <typename ResultType>
void require_identical_roundtrip(const ResultType& encoded, const std::uint8_t* data, std::size_t size) {
    if (!encoded || encoded.value().size() != size) std::abort();
    for (std::size_t i = 0; i < size; ++i)
        if (encoded.value()[i] != data[i]) std::abort();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    auto decoded_envelope = plpt::model::decode_envelope(bytes);
    if (decoded_envelope) {
        auto encoded = plpt::model::encode_envelope(decoded_envelope.value());
        require_identical_roundtrip(encoded, data, size);
    }

    auto decoded_frame = plpt::media::deserialize_video_frame(bytes);
    if (decoded_frame) {
        auto encoded = plpt::media::serialize_video_frame(decoded_frame.value());
        require_identical_roundtrip(encoded, data, size);
    }

    auto decoded_input = plpt::media::deserialize_input_event(bytes);
    if (decoded_input) {
        auto encoded = plpt::media::serialize_input_event(decoded_input.value());
        require_identical_roundtrip(encoded, data, size);
    }
    return 0;
}
