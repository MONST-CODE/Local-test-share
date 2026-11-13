#include "random.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace plpt {
bool SecureRandom::fill(std::span<std::uint8_t> out) {
    std::size_t done = 0;
    while (done < out.size()) {
#ifdef _WIN32
        const auto count = static_cast<ULONG>(std::min(out.size() - done,
            static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));
        if (BCryptGenRandom(nullptr, out.data() + done, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            std::fill(out.begin(), out.end(), 0);
            return false;
        }
        done += count;
#else
        // getrandom attend l'initialisation du CSPRNG ; EINTR est récupérable.
        const auto count = std::min(out.size() - done, std::size_t{256});
        const auto n = ::getrandom(out.data() + done, count, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            std::fill(out.begin(), out.end(), 0);
            return false;
        }
        done += static_cast<std::size_t>(n);
#endif
    }
    return true;
}
} // namespace plpt
