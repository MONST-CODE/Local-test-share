#pragma once
// Inclus uniquement par les adaptateurs Windows. Winsock AVANT windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace plpt::windows {
class WinsockRuntime {
public:
    WinsockRuntime() { WSADATA data{}; error_ = WSAStartup(MAKEWORD(2, 2), &data); }
    ~WinsockRuntime() { if (error_ == 0) WSACleanup(); }
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
    int error() const { return error_; }
private:
    int error_ = 0;
};
inline int ensure_winsock() {
    static const WinsockRuntime runtime; // initialisation thread-safe C++11+.
    return runtime.error();
}
class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE h_;
};
} // namespace plpt::windows
