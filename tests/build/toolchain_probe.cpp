// Native compile/link/runtime and manifest probe. This does not orchestrate builds.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
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
int manifest(const char* path) {
    const HMODULE module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) { std::fprintf(stderr, "manifest: cannot load PE resource, error=%lu\n", GetLastError()); return 1; }
    const HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(24));
    const HGLOBAL data = resource ? LoadResource(module, resource) : nullptr;
    const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    const std::string_view xml(bytes ? bytes : "", bytes ? size : 0);
    const bool valid = xml.find("asInvoker") != std::string_view::npos && xml.find("PerMonitorV2") != std::string_view::npos;
    if (valid) std::printf("manifest: PASS resource #1 asInvoker + PerMonitorV2 %s\n", path);
    else std::fprintf(stderr, "manifest: missing execution-level/DPI resource %s\n", path);
    FreeLibrary(module);
    return valid ? 0 : 1;
}
bool runtime(const char* name) {
    const HMODULE module = GetModuleHandleA(name);
    if (!module) { std::fprintf(stderr, "runtime: required loaded module absent: %s\n", name); return false; }
    char path[32768]{};
    const DWORD size = GetModuleFileNameA(module, path, sizeof(path));
    if (size == 0 || size >= sizeof(path)) return false;
    std::printf("runtime: %s=%s\n", name, path);
    return true;
}
}
#endif
static_assert(__cplusplus >= 202002L);
static_assert(sizeof(void*) == 8);
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--fail") == 0) {
        std::puts("intentional test failure: exit 23");
        return 23;
    }
#ifdef _WIN32
    if (argc == 3 && std::strcmp(argv[1], "--manifest") == 0) return manifest(argv[2]);
#endif
    std::atomic<bool> done{false};
    std::jthread thread([&] { done.store(true); });
    thread.join();
    if (!done.load()) return 1;
#ifdef _WIN32
    unsigned char byte = 0;
    if (BCryptGenRandom(nullptr, &byte, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 1;
    std::printf("toolchain-probe: MSYS2 UCRT64, GNU ABI, clang=%s, C++=%ld, pointer=%zu, libstdc++=%d, GCC=%d, _UCRT=1\n", __clang_version__, __cplusplus, sizeof(void*), __GLIBCXX__, _GLIBCXX_RELEASE);
    if (!runtime("ucrtbase.dll") || !runtime("libstdc++-6.dll") || !runtime("libwinpthread-1.dll")) return 1;
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
