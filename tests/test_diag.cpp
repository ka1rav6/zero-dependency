// tests/test_diag.cpp
//
// Unit tests for the shared diagnostic types (core/diag.hpp). These pin the
// *rendered* output shape because the rest of kap's error plumbing depends on
// it: the messages are what users and test fixtures compare against.

#include "core/diag.hpp"
#include "harness.hpp"

#include <cstddef>
#include <exception>
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

// --- Location rendering edge cases -----------------------------------------------

KAP_TEST("render_location names an unnamed source rather than emitting a bare colon")
{
    // A diagnostic can carry a position without a file name — toml::parse(text)
    // with no source_name does exactly that. The line and column must still
    // appear, in the usual file:line:col shape.
    const kap::diag::Location loc{"", 2, 5};
    KAP_ASSERT_EQ(kap::diag::render_location(loc), "<unknown>:2:5");
});

KAP_TEST("render_location with neither a file nor a position says so")
{
    KAP_ASSERT_EQ(kap::diag::render_location(kap::diag::Location{}), "<unknown>");
});

KAP_TEST("has_position requires both a line and a column")
{
    KAP_ASSERT(kap::diag::Location({"f", 1, 1}).has_position());
    KAP_ASSERT(!kap::diag::Location({"f", 1, -1}).has_position());
    KAP_ASSERT(!kap::diag::Location({"f", -1, 1}).has_position());
    KAP_ASSERT(!kap::diag::Location({"f"}).has_position());
});

KAP_TEST("a report keeps the position even when the source is unnamed")
{
    // The regression: Error's constructor decided whether to show a location
    // by testing the file name alone, so a positioned diagnostic against an
    // unnamed source lost its line and column on the way to the user.
    const kap::diag::Error e(kap::diag::error("expected a value", {"", 2, 5}));

    KAP_ASSERT(e.report().find("2:5") != std::string::npos);
    KAP_ASSERT(e.report().find("expected a value") != std::string::npos);
});

KAP_TEST("a report with no location at all has no location prefix")
{
    const kap::diag::Error e(kap::diag::error("something broke"));
    KAP_ASSERT_EQ(e.report(), "kap: error: something broke\n");
});

// --- Factories -------------------------------------------------------------------

KAP_TEST("the severity factories set their own severity")
{
    KAP_ASSERT(kap::diag::error("x").severity == kap::diag::Severity::Error);
    KAP_ASSERT(kap::diag::warning("x").severity == kap::diag::Severity::Warning);
    KAP_ASSERT(kap::diag::note("x").severity == kap::diag::Severity::Note);
});

KAP_TEST("each severity renders with its own label")
{
    KAP_ASSERT(kap::diag::Error(kap::diag::error("m")).report().find("kap: error: ") == 0);
    KAP_ASSERT(kap::diag::Error(kap::diag::warning("m")).report().find("kap: warning: ") == 0);
    KAP_ASSERT(kap::diag::Error(kap::diag::note("m")).report().find("kap: note: ") == 0);
});

KAP_TEST("the factories carry the message, location, and notes through")
{
    const kap::diag::Diagnostic d =
        kap::diag::error("the message", {"file.toml", 4, 2}, {"first note", "second note"});

    KAP_ASSERT_EQ(d.message, "the message");
    KAP_ASSERT_EQ(d.location.file, "file.toml");
    KAP_ASSERT_EQ(d.location.line, 4);
    KAP_ASSERT_EQ(d.notes.size(), static_cast<std::size_t>(2));
});

// --- Exception behaviour ---------------------------------------------------------

KAP_TEST("every note appears on its own line after the message")
{
    const kap::diag::Error e(
        kap::diag::error("bad key", {"kap.toml", 1, 1}, {"note one", "note two"}));

    const std::string report = e.report();
    KAP_ASSERT(report.find("note one") != std::string::npos);
    KAP_ASSERT(report.find("note two") != std::string::npos);

    // Message first, then notes in order.
    KAP_ASSERT(report.find("bad key") < report.find("note one"));
    KAP_ASSERT(report.find("note one") < report.find("note two"));
});

KAP_TEST("a report always ends with a newline so stderr stays line-oriented")
{
    KAP_ASSERT(kap::diag::Error(kap::diag::error("m")).report().back() == '\n');
    KAP_ASSERT(kap::diag::Error(kap::diag::error("m", {}, {"n"})).report().back() == '\n');
});

KAP_TEST("the structured diagnostic survives alongside the rendered report")
{
    // Callers that want to react to a failure (rather than print it) read the
    // Diagnostic; the rendered text must not be the only copy of the facts.
    const kap::diag::Error e(kap::diag::error("boom", {"kap.toml", 9, 3}));

    KAP_ASSERT_EQ(e.diagnostic().message, "boom");
    KAP_ASSERT_EQ(e.diagnostic().location.line, 9);
    KAP_ASSERT_EQ(e.diagnostic().location.col, 3);
});

KAP_TEST("diag::Error is catchable as a std::exception")
{
    // main() relies on this: one catch(const std::exception&) is the backstop
    // for everything that is not caught as a diag::Error first.
    KAP_ASSERT_THROWS(std::exception, throw kap::diag::Error(kap::diag::error("boom")));
});
