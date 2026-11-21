// Native compile/link/runtime and resource probe. This does not orchestrate builds.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <thread>
#include "resource_probe.hpp"
#ifndef __clang__
#error LLVM clang++ is required
#endif
#if !defined(__x86_64__) && !defined(_M_X64)
#error x86-64 is required
#endif
#ifdef _WIN32
#include <_mingw.h>
#include <windows.h>
#include <bcrypt.h>
#if !defined(__MINGW64__) || !defined(_UCRT)
#error Windows requires MinGW-w64 UCRT headers from MSYS2 UCRT64
#endif
#if defined(_MSC_VER) || defined(_MSVC_STL_VERSION) || defined(_LIBCPP_VERSION) || !defined(__GLIBCXX__)
#error Windows requires the GNU ABI and libstdc++, not MSVC or CLANG64 libc++
#endif
namespace {
bool runtime(const char* name, const std::filesystem::path& prefix) {
    const HMODULE module = GetModuleHandleA(name);
    if (!module) { std::fprintf(stderr, "runtime: required loaded module absent: %s\n", name); return false; }
    char path[32768]{};
    const DWORD size = GetModuleFileNameA(module, path, sizeof(path));
    if (size == 0 || size >= sizeof(path)) return false;
    std::error_code error;
    const auto actual = std::filesystem::canonical(path, error);
    if (error) return false;
    if (!prefix.empty() && actual.parent_path() != prefix) {
        std::fprintf(stderr, "runtime: %s loaded outside selected UCRT64 bin: %s\n", name, path);
        return false;
    }
    std::printf("runtime: %s=%s\n", name, path);
    return true;
}

}
#endif
static_assert(__cplusplus >= 202002L);
static_assert(sizeof(void*) == 8);

namespace {
bool parse_unsigned(const char* text, unsigned long& value) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    value = std::strtoul(text, &end, 10);
    return end != text && *end == '\0';
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--fail") == 0) {
        std::puts("intentional test failure: exit 23");
        return 23;
    }
    if (argc == 5 && std::strcmp(argv[1], "--ico") == 0 && std::strcmp(argv[3], "--expect-entries") == 0) {
        unsigned long count = 0;
        if (parse_unsigned(argv[4], count) && count <= 64 &&
            devlink_probe::probe_ico(argv[2], static_cast<unsigned>(count))) {
            std::printf("ico: PASS %lu entries, all required Windows sizes present\n", count);
            return 0;
        }
        std::fprintf(stderr, "ICO validation failed: %s\n", argv[2]);
        return 1;
    }
    if (argc == 5 && std::strcmp(argv[1], "--pe-icons") == 0 && std::strcmp(argv[3], "--expect-icons") == 0) {
        unsigned long count = 0;
        std::string reason;
        if (parse_unsigned(argv[4], count) && count <= 64 &&
            devlink_probe::probe_pe_icons(argv[2], static_cast<unsigned>(count), &reason)) {
            std::printf("pe-icons: PASS RT_GROUP_ICON + %lu matching RT_ICON resources\n", count);
            return 0;
        }
        std::fprintf(stderr, "PE icon validation failed: %s (%s)\n", argv[2], reason.empty() ? "invalid expected icon count" : reason.c_str());
        return 1;
    }
    if (argc == 5 && std::strcmp(argv[1], "--manifest") == 0 && std::strcmp(argv[3], "--expect-version") == 0) {
        if (devlink_probe::probe_manifest(argv[2], argv[4])) {
            std::printf("manifest: PASS resource #1 version=%s asInvoker + PerMonitorV2 %s\n", argv[4], argv[2]);
            return 0;
        }
        std::fprintf(stderr, "PE manifest validation failed: %s\n", argv[2]);
        return 1;
    }
#ifdef _WIN32
    if (argc == 3 && std::strcmp(argv[1], "--expect-compiler") == 0) {
        std::error_code canonical_error;
        const char* root = std::getenv("DEVLINK_MSYS2_ROOT");
        if (root == nullptr || *root == '\0') root = std::getenv("PLPT_MSYS2_ROOT");
        if (root == nullptr || *root == '\0') {
            std::fprintf(stderr, "toolchain: DEVLINK_MSYS2_ROOT is required for provenance proof\n");
            return 1;
        }
        const auto prefix = std::filesystem::canonical(std::filesystem::path(root) / "ucrt64" / "bin", canonical_error);
        if (canonical_error) {
            std::fprintf(stderr, "toolchain: selected UCRT64 bin is not canonicalizable\n");
            return 1;
        }
        const auto executable = std::filesystem::canonical(argv[2], canonical_error);
        if (canonical_error || executable.parent_path() != prefix || executable.filename() != "clang++.exe") {
            std::fprintf(stderr, "toolchain: compiler is not <DEVLINK_MSYS2_ROOT>/ucrt64/bin/clang++.exe\n");
            return 1;
        }
        std::printf("toolchain-probe: selected compiler=%s\n", executable.string().c_str());
        unsigned char byte = 0;
        if (BCryptGenRandom(nullptr, &byte, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 1;
        std::printf("toolchain-probe: MSYS2 UCRT64, GNU ABI, clang=%s, C++=%ld, pointer=%zu, libstdc++=%d, GCC=%d, _UCRT=1\n",
            __clang_version__, __cplusplus, sizeof(void*), __GLIBCXX__, _GLIBCXX_RELEASE);
        if (!runtime("ucrtbase.dll", {}) || !runtime("libstdc++-6.dll", prefix) ||
            !runtime("libwinpthread-1.dll", prefix)) return 1;
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--manifest") == 0) {
        std::fprintf(stderr, "manifest: --expect-version <major.minor.patch.build> is required\n");
        return 1;
    }
#else
#ifndef __linux__
#error Native Linux is required
#endif
#endif
    std::atomic<bool> done{false};
    std::jthread thread([&] { done.store(true); });
    thread.join();
    if (!done.load()) return 1;
#ifdef _WIN32
    std::fprintf(stderr, "toolchain: --expect-compiler <path> is required for provenance proof\n");
    return 1;
#else
    std::printf("toolchain-probe: native Linux x64, clang=%s, C++=%ld, pointer=%zu\n", __clang_version__, __cplusplus, sizeof(void*));
#ifdef __GLIBCXX__
    std::printf("libstdc++ headers: %d\n", __GLIBCXX__);
#endif
    return 0;
#endif
}
