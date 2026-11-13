// Capability probe for the exact UCRT64 clang++ libFuzzer runtime.
//
// The doctor must never report "libFuzzer available" from the presence of an
// archive alone. This translation unit is compiled and linked with exactly the
// same two-step recipe as the real fuzz target (-fsanitize=fuzzer-no-link for
// compilation, -fsanitize=fuzzer, optionally plus -fsanitize=address, at link
// time) and the resulting executable is executed with -runs=1. Only a probe
// that really links and really runs turns the capability flag on; anything
// else leaves the fuzz operation a recorded SKIP, never a fabricated PASS.
//
// No application logic lives here: the harness touches only its own input bytes.

#include <cstddef>
#include <cstdint>

namespace {

// Deterministic, side-effect-free reduction of the fuzz input.
std::uint64_t Reduce(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint64_t digest = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    digest ^= static_cast<std::uint64_t>(data[i]);
    digest *= 1099511628211ull;
  }
  return digest;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (data == nullptr) return 0;
  // The sentinel is unreachable for the single-byte inputs used by -runs=1,
  // so the probe proves execution without crashing the doctor.
  return Reduce(data, size) == 0xDEADBEEFDEADBEEFull ? 1 : 0;
}
