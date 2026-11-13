#pragma once
#include <cstdio>
#include <exception>
#include <stdexcept>
namespace plpt_test {
inline unsigned checks = 0, failures = 0, cases = 0, failed_cases = 0;
inline bool report(bool ok, const char* expr, const char* file, int line) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s:%d: %s\n", file, line, expr); }
    return ok;
}
struct RequirementFailure {};
template <typename F> void run(const char* name, F test) {
    ++cases;
    const auto before = failures;
    try { test(); }
    catch (const RequirementFailure&) {}
    catch (const std::exception& e) { ++failures; std::printf("EXCEPTION %s: %s\n", name, e.what()); }
    catch (...) { ++failures; std::printf("EXCEPTION %s: unknown\n", name); }
    if (before != failures) ++failed_cases;
    std::printf("[%s] %s\n", before == failures ? "PASS" : "FAIL", name);
}
inline int finish(const char* suite) {
    std::printf("%s: %u cases, %u checks, %u failures (%u failed cases)\n", suite, cases, checks, failures, failed_cases);
    return failures == 0 ? 0 : 1;
}
}
#define CHECK(expr) (void)::plpt_test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a,b) CHECK((a) == (b))
#define REQUIRE(expr) do { if (!::plpt_test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)) throw ::plpt_test::RequirementFailure{}; } while(false)
#define CASE(name) ::plpt_test::run(#name, name)
