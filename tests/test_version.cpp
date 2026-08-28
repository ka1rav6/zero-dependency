// tests/test_version.cpp
//
// Milestone 0 unit tests: the version constants in core/version.hpp must be
// internally consistent, and the banner string must report what the rest of
// the toolchain (main.cpp, CMake's PASS_REGULAR_EXPRESSION) promises.
//
// We compose the expected string from the numeric pieces rather than yet
// another hardcoded literal, so a version bump either updates every constant
// in one place or a test fails and reminds us.

#include "core/version.hpp"
#include "harness.hpp"

#include <string>

KAP_TEST("version string is composed of the numeric pieces")
{
    // Building the expectation from the constants themselves turns a typo in
    // kVersionString into a test failure instead of a silent mismatch.
    const std::string composed = std::to_string(kap::kVersionMajor) + "." +
                                 std::to_string(kap::kVersionMinor) + "." +
                                 std::to_string(kap::kVersionPatch);

    KAP_ASSERT_EQ(kap::kVersionString, composed);
});

KAP_TEST("program name is stable")
{
    KAP_ASSERT_EQ(kap::kProgramName, "kap");
});

KAP_TEST("version pieces are non-negative")
{
    // A negative version number would be nonsense; guard the free functions
    // that later milestones will build on top of these constants.
    KAP_ASSERT(kap::kVersionMajor >= 0);
    KAP_ASSERT(kap::kVersionMinor >= 0);
    KAP_ASSERT(kap::kVersionPatch >= 0);
});