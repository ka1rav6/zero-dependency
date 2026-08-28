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

// Assert equality between two values.
//
// The comparison uses the values' own `operator==`, NOT their rendered text.
// An earlier version compared to_string_display(a) against to_string_display(b),
// which meant the assertion passed whenever two values merely *printed* the
// same — std::string("1") "equalled" the integer 1, and any type whose display
// form loses information (a truncated float, a pointer shown as its target)
// could hide a real mismatch. Rendering is now only used to build the failure
// message, which is what it is actually good for.
//
// Both operands are bound *by value* rather than by reference. Binding
// `const auto&` to an expression like `doc.get("k")->str` would reference a
// member of an already-destroyed temporary std::optional: lifetime extension
// applies only when a reference binds directly to a prvalue, not when it binds
// to something reachable *through* one. Copying also makes the guarantee below
// real — each operand expression is evaluated exactly once, so an assertion on
// a side-effecting call cannot double-fire.
#define KAP_ASSERT_EQ(a, b)                                                                        \
    do {                                                                                           \
        const auto kap_test_lhs = (a);                                                             \
        const auto kap_test_rhs = (b);                                                              \
        if (!(kap_test_lhs == kap_test_rhs)) {                                                     \
            ::kap_test::fail_test(__FILE__,                                                        \
                                  __LINE__,                                                        \
                                  std::string("expected '") + #a + "' == '" + #b + "' but got '" + \
                                      ::kap_test::to_string_display(kap_test_lhs) + "' vs '" +     \
                                      ::kap_test::to_string_display(kap_test_rhs) + "'");          \
        }                                                                                          \
    } while (0)

// The inverse of KAP_ASSERT_EQ, for "these must not collapse into each other"
// properties (e.g. two different words must quote differently).
#define KAP_ASSERT_NE(a, b)                                                                        \
    do {                                                                                           \
        const auto kap_test_lhs = (a);                                                             \
        const auto kap_test_rhs = (b);                                                              \
        if (kap_test_lhs == kap_test_rhs) {                                                        \
            ::kap_test::fail_test(__FILE__,                                                        \
                                  __LINE__,                                                        \
                                  std::string("expected '") + #a + "' != '" + #b +                 \
                                      "' but both are '" +                                         \
                                      ::kap_test::to_string_display(kap_test_lhs) + "'");           \
        }                                                                                          \
    } while (0)

// Assert that `expr` throws `exception_type`.
//
// kap's whole error story is "throw a diag::Error with a location attached"
// (core/diag.hpp), so nearly every parser test needs to check a failure path.
// Written by hand that is six lines of try/catch boilerplate per case, and the
// easy mistake — forgetting to assert that the throw actually happened — makes
// the test pass even when nothing is thrown. This macro cannot be written
// wrongly that way.
//
// Note the deliberate ordering of the catch clauses: an unrelated exception is
// reported as its own distinct failure ("threw the wrong type") rather than
// being counted as success.
#define KAP_ASSERT_THROWS(exception_type, expr)                                                    \
    do {                                                                                           \
        bool kap_test_threw = false;                                                               \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (const exception_type&) {                                                          \
            kap_test_threw = true;                                                                 \
        } catch (const std::exception& kap_test_other) {                                           \
            ::kap_test::fail_test(__FILE__,                                                        \
                                  __LINE__,                                                        \
                                  std::string("expected '") + #expr + "' to throw " +              \
                                      #exception_type + " but it threw something else: " +         \
                                      kap_test_other.what());                                      \
        }                                                                                          \
        if (!kap_test_threw) {                                                                     \
            ::kap_test::fail_test(__FILE__,                                                        \
                                  __LINE__,                                                        \
                                  std::string("expected '") + #expr + "' to throw " +              \
                                      #exception_type + " but it returned normally");              \
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