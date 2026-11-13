#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "engine/protocol/envelope.hpp"
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto decoded = plpt::model::decode_envelope({data, size});
    if (decoded) {
        auto encoded = plpt::model::encode_envelope(decoded.value());
        if (!encoded || encoded.value().size() != size) std::abort();
        for (std::size_t i = 0; i < size; ++i) if (encoded.value()[i] != data[i]) std::abort();
    }
    return 0;
}
