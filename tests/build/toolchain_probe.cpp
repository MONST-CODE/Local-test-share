// Native compile + link + execution probe, not a build engine.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#ifndef __clang__
#error LLVM clang++ is required
#endif
#if !defined(__x86_64__) && !defined(_M_X64)
#error x86-64 is required
#endif
#ifdef _WIN32
#if !defined(_MSC_VER) || defined(__MINGW32__)
#error Windows requires the Microsoft ABI/SDK/STL, not MinGW
#endif
#include <windows.h>
#include <bcrypt.h>
#endif
static_assert(__cplusplus >= 202002L);
static_assert(sizeof(void*) == 8);
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--fail") == 0) {
        std::puts("intentional test failure: exit 23");
        return 23;
    }
    std::atomic<bool> done{false};
    std::jthread thread([&] { done.store(true); });
    thread.join();
    if (!done.load()) return 1;
#ifdef _WIN32
    unsigned char byte = 0;
    if (BCryptGenRandom(nullptr, &byte, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 1;
    std::printf("toolchain-probe: native Windows x64, Microsoft ABI, clang=%s, MSVC=%d, STL=%d, C++=%ld\n", __clang_version__, _MSC_VER, _MSVC_STL_VERSION, __cplusplus);
#else
#ifndef __linux__
#error Native Linux is required
#endif
    std::printf("toolchain-probe: native Linux x64, clang=%s, C++=%ld, pointer=%zu\n", __clang_version__, __cplusplus, sizeof(void*));
#ifdef __GLIBCXX__
    std::printf("libstdc++ headers: %d\n", __GLIBCXX__);
#endif
#endif
    return 0;
}
