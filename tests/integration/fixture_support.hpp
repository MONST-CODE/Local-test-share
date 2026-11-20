#pragma once
// DevLink — tests/integration/fixture_support.hpp
// Shared by the real-browser and share pipeline tests (Windows):
//  - FixtureService: launches the REAL devlink-demo-service executable (the
//    local web application under test) with a random run id, reads its port.
//  - Canary: a loopback TCP listener on a port that is never allowlisted; any
//    connection it receives is a policy leak.
//  - read_run_band / square_colour: read the fixture's machine-readable pixels.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "engine/core/random.hpp"
#include "engine/media/jpeg_decoder.hpp"
#include "engine/platform/socket.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace plpt_fixture {

inline std::string random_run_id() {
    plpt::SecureRandom random;
    std::array<std::uint8_t, 8> bytes{};
    if (!random.fill(bytes)) return "0123456789abcdef";
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

inline std::filesystem::path executable_dir() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, n)).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

#if defined(_WIN32)
class FixtureService {
public:
    FixtureService() = default;
    FixtureService(const FixtureService&) = delete;
    FixtureService& operator=(const FixtureService&) = delete;
    ~FixtureService() { stop(); }

    bool start(const std::string& run_id) {
        run_id_ = run_id;
        const auto exe = executable_dir() / "devlink-demo-service.exe";
        if (!std::filesystem::exists(exe)) return false;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write;
        si.hStdError = write;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        std::wstring cmd = L"\"" + exe.wstring() + L"\" --quiet --port 0 --run-id " +
            std::wstring(run_id.begin(), run_id.end());
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(write);
        if (!ok) {
            CloseHandle(read);
            return false;
        }
        process_ = pi.hProcess;
        CloseHandle(pi.hThread);
        // First line: "port=<n>"
        std::string line;
        char c = 0;
        DWORD got = 0;
        while (line.size() < 64 && ReadFile(read, &c, 1, &got, nullptr) && got == 1 && c != '\n') line.push_back(c);
        output_ = read;
        if (line.rfind("port=", 0) != 0) return false;
        port_ = static_cast<std::uint16_t>(std::stoul(line.substr(5)));
        return port_ != 0;
    }
    void stop() {
        if (process_) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (output_) {
            CloseHandle(output_);
            output_ = nullptr;
        }
    }
    std::uint16_t port() const { return port_; }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    const std::string& run_id() const { return run_id_; }

private:
    HANDLE process_ = nullptr;
    HANDLE output_ = nullptr;
    std::uint16_t port_ = 0;
    std::string run_id_;
};
#endif

// Loopback listener that counts every connection attempt it receives.
class Canary {
public:
    bool start() {
        auto s = plpt::Socket::create(plpt::TransportProtocol::Tcp, plpt::IpFamily::Ipv4);
        if (!s) return false;
        socket_ = std::move(s).value();
        if (!socket_.bind(plpt::IpAddress::loopback(plpt::IpFamily::Ipv4), plpt::Port{0}) || !socket_.listen(16) ||
            !socket_.set_nonblocking(true))
            return false;
        auto p = socket_.bound_port();
        if (!p) return false;
        port_ = p.value().value;
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                plpt::IpAddress peer;
                plpt::Port peer_port{};
                auto accepted = socket_.accept(&peer, &peer_port);
                if (accepted) {
                    hits_.fetch_add(1);
                    accepted.value().close();
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });
        return true;
    }
    ~Canary() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        socket_.close();
    }
    std::uint16_t port() const { return port_; }
    unsigned hits() const { return hits_.load(); }

private:
    plpt::Socket socket_;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> hits_{0};
};

struct Rgb { int r = 0, g = 0, b = 0; };

inline Rgb average(const plpt::media::DecodedImage& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h) {
    long long r = 0, g = 0, b = 0, n = 0;
    for (std::uint32_t y = y0; y < y0 + h && y < img.height; ++y)
        for (std::uint32_t x = x0; x < x0 + w && x < img.width; ++x) {
            const auto* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
            r += p[0]; g += p[1]; b += p[2]; ++n;
        }
    if (n == 0) return {};
    return {static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n)};
}

// Decodes the 64-cell run band (cells of 20x24 CSS px at a 1280 px viewport).
// Returns an empty string if any cell is neither clearly white nor black.
inline std::string read_run_band(const plpt::media::DecodedImage& img) {
    if (img.width < 1280 || img.height < 24) return {};
    const double scale = img.width / 1280.0;
    std::string hex;
    int nibble = 0;
    for (int bit = 0; bit < 64; ++bit) {
        const auto x = static_cast<std::uint32_t>((bit * 20 + 6) * scale);
        const auto c = average(img, x, static_cast<std::uint32_t>(6 * scale), static_cast<std::uint32_t>(8 * scale),
            static_cast<std::uint32_t>(12 * scale));
        const int lum = (c.r + c.g + c.b) / 3;
        if (lum > 40 && lum < 215) return {};
        nibble = (nibble << 1) | (lum >= 215 ? 1 : 0);
        if (bit % 4 == 3) {
            hex.push_back("0123456789abcdef"[nibble]);
            nibble = 0;
        }
    }
    return hex;
}

// 0 red, 1 green, 2 blue, 3 yellow, -1 unknown (the fixture square, clicks % 4)
inline int square_colour(const plpt::media::DecodedImage& img) {
    const double scale = img.width / 1280.0;
    const auto c = average(img, static_cast<std::uint32_t>(90 * scale), static_cast<std::uint32_t>(150 * scale),
        static_cast<std::uint32_t>(100 * scale), static_cast<std::uint32_t>(100 * scale));
    if (c.r > 160 && c.g < 80 && c.b < 80) return 0;
    if (c.g > 130 && c.r < 80 && c.b < 80) return 1;
    if (c.b > 150 && c.r < 80 && c.g < 110) return 2;
    if (c.r > 170 && c.g > 160 && c.b < 90) return 3;
    return -1;
}

} // namespace plpt_fixture
