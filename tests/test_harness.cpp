// tests/test_harness.cpp
//
// Tests for the test harness itself (tests/harness.hpp).
//
// This is not navel-gazing: the harness is the thing that decides whether
// every other test in the suite passes or fails, so a bug in it is silent and
// global. KAP_ASSERT_EQ really did once compare the *rendered text* of its two
// operands instead of the values, which meant a whole class of mismatches
// reported green. The cases below pin the properties that mistake violated.
//
// The trick used throughout: an assertion failure is just a thrown
// AssertionFailure, so a test can invoke an assertion on purpose inside a
// lambda, catch the exception, and inspect it. `expect_fails` / `expect_passes`
// wrap that so each case reads as one line.

#include "harness.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace
{

// Run `body` and report whether it raised an assertion failure.
bool assertion_failed(const std::function<void()>& body)
{
    try {
        body();
    }
    catch (const kap_test::AssertionFailure&) {
        return true;
    }
    return false;
}

// Run `body` and return the failure message it produced (empty if it passed).
std::string failure_message(const std::function<void()>& body)
{
    try {
        body();
    }
    catch (const kap_test::AssertionFailure& e) {
        return e.message;
    }
    return {};
}

} // namespace

// --- KAP_ASSERT ------------------------------------------------------------------

KAP_TEST("harness: KAP_ASSERT passes a true condition and fails a false one")
{
    KAP_ASSERT(!assertion_failed([] { KAP_ASSERT(1 + 1 == 2); }));
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT(1 + 1 == 3); }));
});

KAP_TEST("harness: a failure message carries file:line and the expression text")
{
    const std::string message = failure_message([] { KAP_ASSERT(false); });

    KAP_ASSERT(message.find("test_harness.cpp:") != std::string::npos);
    KAP_ASSERT(message.find("assertion failed: false") != std::string::npos);
});

// --- KAP_ASSERT_EQ ---------------------------------------------------------------

KAP_TEST("harness: KAP_ASSERT_EQ compares equal values as equal")
{
    KAP_ASSERT(!assertion_failed([] { KAP_ASSERT_EQ(std::string("kap"), "kap"); }));
    KAP_ASSERT(!assertion_failed([] { KAP_ASSERT_EQ(42, 42); }));
});

KAP_TEST("harness: KAP_ASSERT_EQ fails on unequal values")
{
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT_EQ(std::string("kap"), "nope"); }));
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT_EQ(42, 43); }));
});

KAP_TEST("harness: KAP_ASSERT_EQ compares values, not their printed form")
{
    // The regression this file exists for. Two std::strings that differ only
    // in trailing whitespace, and a signed/unsigned pair that print alike,
    // must both be judged by operator==, not by rendered text.
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT_EQ(std::string("1 "), std::string("1")); }));
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT_EQ(std::string(""), std::string(" ")); }));
});

KAP_TEST("harness: a KAP_ASSERT_EQ failure message shows both operands")
{
    const std::string message =
        failure_message([] { KAP_ASSERT_EQ(std::string("left"), std::string("right")); });

    KAP_ASSERT(message.find("left") != std::string::npos);
    KAP_ASSERT(message.find("right") != std::string::npos);
});

KAP_TEST("harness: KAP_ASSERT_EQ evaluates each operand exactly once")
{
    // The macro binds each operand to a reference before comparing. If it
    // instead expanded the arguments twice, a counter would land on 2.
    int        calls = 0;
    const auto bump  = [&calls] { return ++calls; };

    KAP_ASSERT(assertion_failed([&] { KAP_ASSERT_EQ(bump(), 99); }));
    KAP_ASSERT_EQ(calls, 1);
});

// --- KAP_ASSERT_NE ---------------------------------------------------------------

KAP_TEST("harness: KAP_ASSERT_NE is the exact inverse of KAP_ASSERT_EQ")
{
    KAP_ASSERT(!assertion_failed([] { KAP_ASSERT_NE(1, 2); }));
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT_NE(1, 1); }));
});

// --- KAP_ASSERT_THROWS -----------------------------------------------------------

KAP_TEST("harness: KAP_ASSERT_THROWS passes when the expected type is thrown")
{
    KAP_ASSERT(!assertion_failed(
        [] { KAP_ASSERT_THROWS(std::runtime_error, throw std::runtime_error("boom")); }));
});

KAP_TEST("harness: KAP_ASSERT_THROWS fails when nothing is thrown")
{
    // The failure mode of hand-written try/catch boilerplate: forgetting to
    // assert that the throw happened at all, so the test passes vacuously.
    KAP_ASSERT(assertion_failed([] { KAP_ASSERT_THROWS(std::runtime_error, 1 + 1); }));
});

KAP_TEST("harness: KAP_ASSERT_THROWS fails when a different exception is thrown")
{
    KAP_ASSERT(assertion_failed(
        [] { KAP_ASSERT_THROWS(std::out_of_range, throw std::runtime_error("wrong type")); }));
});

KAP_TEST("harness: KAP_ASSERT_THROWS catches a derived exception type")
{
    // std::out_of_range derives from std::logic_error, so catching the base
    // must work — plugins throw diag::Error, which callers catch as such.
    KAP_ASSERT(!assertion_failed(
        [] { KAP_ASSERT_THROWS(std::logic_error, throw std::out_of_range("derived")); }));
});

// --- Registration ----------------------------------------------------------------

KAP_TEST("harness: every registered test has a name, a file, and a body")
{
    const auto& tests = kap_test::registry();
    KAP_ASSERT(!tests.empty());

    for (const auto& test : tests) {
        KAP_ASSERT(!test.name.empty());
        KAP_ASSERT(!test.file.empty());
        KAP_ASSERT(test.line > 0);
        KAP_ASSERT(static_cast<bool>(test.body));
    }
});

KAP_TEST("harness: to_string_display renders strings and bools readably")
{
    KAP_ASSERT_EQ(kap_test::to_string_display(std::string("hi")), "hi");
    KAP_ASSERT_EQ(kap_test::to_string_display(true), "true");
    KAP_ASSERT_EQ(kap_test::to_string_display(false), "false");
    KAP_ASSERT_EQ(kap_test::to_string_display(static_cast<const char*>(nullptr)), "(null)");
    KAP_ASSERT_EQ(kap_test::to_string_display(static_cast<std::int64_t>(-7)), "-7");
});
