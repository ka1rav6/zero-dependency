#pragma once

// tests/harness.hpp
//
// The zero-dependency unit test harness. External test frameworks are banned
// (AGENTS.md §6 and design doc §9 require std/POSIX-only tooling), so this
// header *is* the framework:
//
//     KAP_TEST("name of the test") {
//         KAP_ASSERT(some_condition);
//         KAP_ASSERT_EQ(a_value, b_value);
//     });
//
// The KAP_TEST macro registers a closure in a global registry (via a static
// object whose constructor runs at program start); tests/test_main.cpp is the
// runner that executes every registered test and reports pass/fail with a
// non-zero exit code on any failure.
//
// Why a macro instead of a plain function? The preprocessor splices
// __FILE__/__LINE__ into every assertion, so failures report exactly where they
// happened — no tracing through stacks for beginners.
//
// Note: the macro form ends with `});` — the captured tests in
// test_*.cpp all follow this shape.

#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace kap_test
{

// One registered unit test. `file`/`line` are filled in by the KAP_TEST macro
// so a failure can point straight at the offending source line.
struct TestCase
{
    std::string           name;
    std::string           file;
    int                   line;
    std::function<void()> body;
};

// The global registry of tests. `inline` gives us exactly one registry shared
// by every translation unit that includes this header (C++17 inline
// variables), so each test file self-registers without extra wiring.
inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

// Thrown by a failed assertion; the runner catches it and counts the failure.
struct AssertionFailure : std::exception
{
    std::string message;

    // Build the exception from a fully-formatted diagnostic line. `explicit`
    // so callers cannot accidentally construct one from a bare C-string.
    explicit AssertionFailure(std::string msg) : message(std::move(msg)) {}

    const char* what() const noexcept override
    {
        return message.c_str();
    }
};

// Throw a formatted assertion failure. Kept as a helper so every KAP_ASSERT*
// macro can report its file/line/expression uniformly.
[[noreturn]] inline void fail_test(const char* file, int line, const std::string& detail)
{
    throw AssertionFailure{std::string(file) + ":" + std::to_string(line) + ": " + detail};
}

// Register a test; used only by the Registrar struct below.
inline void register_test(std::string name, std::string file, int line, std::function<void()> body)
{
    registry().push_back(TestCase{std::move(name), std::move(file), line, std::move(body)});
}

// Tiny static object created by KAP_TEST; its constructor self-registers the
// test so the runner needs no explicit list of what to run.
struct Registrar
{
    Registrar(std::string name, std::string file, int line, std::function<void()> body)
    {
        register_test(std::move(name), std::move(file), line, std::move(body));
    }
};

// --- Stringify helpers ---------------------------------------------------------
// Used by KAP_ASSERT_EQ to render values in failure messages. Strings and
// C-strings print verbatim (handled as content, not pointers); everything else
// falls back to std::to_string.
inline const std::string& to_string_display(const std::string& s)
{
    return s;
}

inline std::string to_string_display(const char* s)
{
    return s == nullptr ? "(null)" : s;
}

inline std::string to_string_display(bool b)
{
    return b ? "true" : "false";
}

template <typename T> std::string to_string_display(const T& v)
{
    return std::to_string(v);
}

// --- Assertions -----------------------------------------------------------------

// Assert a boolean condition. Message includes the source text of the
// expression so failures read like "assertion failed: x == 3".
#define KAP_ASSERT(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::kap_test::fail_test(__FILE__, __LINE__, std::string("assertion failed: ") + #cond);  \
        }                                                                                          \
    } while (0)

// Assert (content) equality between two values. Values are rendered through
// to_string_display, so two C-strings compare by content, not by pointer.
#define KAP_ASSERT_EQ(a, b)                                                                        \
    do {                                                                                           \
        const auto&       kap_test_lhs   = (a);                                                    \
        const auto&       kap_test_rhs   = (b);                                                    \
        const std::string kap_test_left  = ::kap_test::to_string_display(kap_test_lhs);            \
        const std::string kap_test_right = ::kap_test::to_string_display(kap_test_rhs);            \
        if (kap_test_left != kap_test_right) {                                                     \
            ::kap_test::fail_test(__FILE__,                                                        \
                                  __LINE__,                                                        \
                                  std::string("expected '") + #a + "' == '" + #b + "' but got '" + \
                                      kap_test_left + "' vs '" + kap_test_right + "'");            \
        }                                                                                          \
    } while (0)

} // namespace kap_test

// --- KAP_TEST macro ---------------------------------------------------------------
// Self-registering test. Usage:
//
//     KAP_TEST("my test") {
//         ... assertions ...
//     });
#define KAP_TEST_CONCAT_IMPL(a, b) a##b
#define KAP_TEST_CONCAT(a, b) KAP_TEST_CONCAT_IMPL(a, b)

#define KAP_TEST(name)                                                                             \
    static ::kap_test::Registrar KAP_TEST_CONCAT(kap_registrar_, __LINE__)(       \
        (name), __FILE__, __LINE__, []()
// (end of KAP_TEST — the caller-supplied `{ ... });` closes the [] lambda and
//  the Registrar(...) construction. See the usage examples near the top.)