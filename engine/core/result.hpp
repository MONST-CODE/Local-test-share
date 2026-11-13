#pragma once
#include <cstdint>
#include <string_view>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace plpt {
enum class ErrorCode {
    Ok, InvalidArgument, NotFound, AlreadyExists, PermissionDenied, Expired,
    Replayed, RateLimited, CapacityExceeded, InvalidState, ProtocolViolation,
    UnsupportedVersion, Unsupported, IoError, PlatformError, Cancelled, Internal,
    // Réseau réel : trois états qui ne sont ni des succès ni des fautes.
    WouldBlock, TimedOut, Disconnected, Unauthorized, TooLarge
};
inline std::string_view to_string(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::Expired: return "Expired";
    case ErrorCode::Replayed: return "Replayed";
    case ErrorCode::RateLimited: return "RateLimited";
    case ErrorCode::CapacityExceeded: return "CapacityExceeded";
    case ErrorCode::InvalidState: return "InvalidState";
    case ErrorCode::ProtocolViolation: return "ProtocolViolation";
    case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::PlatformError: return "PlatformError";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::WouldBlock: return "WouldBlock";
    case ErrorCode::TimedOut: return "TimedOut";
    case ErrorCode::Disconnected: return "Disconnected";
    case ErrorCode::Unauthorized: return "Unauthorized";
    case ErrorCode::TooLarge: return "TooLarge";
    }
    return "Unknown";
}
struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message; // Diagnostic technique, jamais du contenu non fiable.
    std::string context;
    std::int64_t native_code = 0;
};
inline Error checked_error(Error e) {
    if (e.code == ErrorCode::Ok) throw std::invalid_argument("failure(Ok)");
    return e;
}

// C++20 : alternative bornée à std::expected (C++23). Un mauvais accès est une
// erreur de programmation explicite, jamais un déréférencement d'optional vide.
template <typename T> class [[nodiscard]] Result {
public:
    static Result success(T v) { return Result(std::move(v)); }
    static Result failure(Error e) { return Result(checked_error(std::move(e))); }
    bool has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    const T& value() const& { return value_.value(); }
    T& value() & { return value_.value(); }
    T&& value() && { return std::move(value_.value()); }
    const Error& error() const noexcept { return error_; }
private:
    explicit Result(T v) : value_(std::move(v)) {}
    explicit Result(Error e) : error_(std::move(e)) {}
    std::optional<T> value_;
    Error error_;
};
template <> class [[nodiscard]] Result<void> {
public:
    static Result success() { return Result(); }
    static Result failure(Error e) { return Result(checked_error(std::move(e))); }
    bool has_value() const noexcept { return error_.code == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return has_value(); }
    const Error& error() const noexcept { return error_; }
private:
    Result() = default;
    explicit Result(Error e) : error_(std::move(e)) {}
    Error error_;
};
} // namespace plpt
