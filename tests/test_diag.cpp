// tests/test_diag.cpp
//
// Unit tests for the shared diagnostic types (core/diag.hpp). These pin the
// *rendered* output shape because the rest of kap's error plumbing depends on
// it: the messages are what users and test fixtures compare against.

#include "core/diag.hpp"
#include "harness.hpp"

#include <string>
#include <utility>

KAP_TEST("render_location formats file, line, and column")
{
    const kap::diag::Location loc{"kap.toml", 3, 7};
    KAP_ASSERT_EQ(kap::diag::render_location(loc), "kap.toml:3:7");
});

KAP_TEST("render_location without a position falls back to the file name")
{
    const kap::diag::Location loc{"kap.toml"};
    KAP_ASSERT_EQ(kap::diag::render_location(loc), "kap.toml");
});

KAP_TEST("error report carries the severity, location, message, and notes")
{
    kap::diag::Diagnostic d;
    d.message  = "boom";
    d.location = {"kap.toml", 1, 2};
    d.notes.push_back("hint: maybe not");

    const kap::diag::Error e(std::move(d));
    const std::string      report = e.report();

    KAP_ASSERT(report.find("kap: error: kap.toml:1:2: boom") != std::string::npos);
    KAP_ASSERT(report.find("hint: maybe not") != std::string::npos);
});

KAP_TEST("warning diagnostics render with a warning label")
{
    kap::diag::Diagnostic d;
    d.severity = kap::diag::Severity::Warning;
    d.message  = "careful now";

    const kap::diag::Error e(std::move(d));
    KAP_ASSERT(e.report().find("kap: warning: careful now") != std::string::npos);
});

KAP_TEST("what() exposes the same rendered report")
{
    kap::diag::Diagnostic d;
    d.message = "plain";

    const kap::diag::Error e(std::move(d));
    KAP_ASSERT(e.report() == e.what());
});