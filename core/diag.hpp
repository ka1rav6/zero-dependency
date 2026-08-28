#pragma once

// core/diag.hpp
//
// Shared diagnostic infrastructure (design doc Milestone 1): error types with
// a structured source location and rich, human-readable rendering. Every
// subsystem (TOML parser, CLI parser, KPL interpreter, executor) reports
// failures through these types so user-facing errors look the same everywhere:
//
//     kap: error: <file>:<line>:<col>: <message>
//           note: <extra context>
//
// All of kap is std/POSIX only, so there is no third-party exception library;
// a plain std::exception subclass carries the Diagnostic.

#include <exception>
#include <string>
#include <vector>

namespace kap
{
namespace diag
{

// How serious a diagnostic is. Only Error currently aborts a command, but
// Warning/Note exist so later milestones can surface non-fatal problems (e.g.
// a config key that is about to be deprecated).
enum class Severity
{
    Note,
    Warning,
    Error,
};

// Where a problem happened: a file (or a synthetic name like "<argv>"), and
// optionally a 1-based line/column. line == -1 means "whole file".
struct Location
{
    std::string file;
    int         line = -1;
    int         col  = -1;

    // Whether the location points at a specific character (vs. the whole file).
    bool has_position() const
    {
        return line >= 0 && col >= 0;
    }
};

// A single reportable problem, possibly with follow-up notes.
struct Diagnostic
{
    Severity                 severity = Severity::Error;
    std::string              message;
    Location                 location;
    std::vector<std::string> notes;
};

// Factory helpers keep callers terse and -Wall-clean: because every field is
// supplied in declaration order, GCC's -Wmissing-field-initializers has
// nothing to complain about and it reads better than a four-field aggregate.
//
// One per severity, so the Severity enum is actually reachable from code
// rather than requiring callers to fill a Diagnostic in by hand.
inline Diagnostic
error(std::string message, Location location = {}, std::vector<std::string> notes = {})
{
    return Diagnostic{.severity = Severity::Error,
                      .message  = std::move(message),
                      .location = std::move(location),
                      .notes    = std::move(notes)};
}

inline Diagnostic
warning(std::string message, Location location = {}, std::vector<std::string> notes = {})
{
    return Diagnostic{.severity = Severity::Warning,
                      .message  = std::move(message),
                      .location = std::move(location),
                      .notes    = std::move(notes)};
}

inline Diagnostic
note(std::string message, Location location = {}, std::vector<std::string> notes = {})
{
    return Diagnostic{.severity = Severity::Note,
                      .message  = std::move(message),
                      .location = std::move(location),
                      .notes    = std::move(notes)};
}

// Renders a location as "<file>:<line>:<col>" (or just "<file>" when there is
// no position). Useful for composing messages inside notes.
std::string render_location(const Location& loc);

// An exception that wraps a Diagnostic. `what()` returns a pre-rendered,
// multi-line report so callers can just catch and print.
class Error : public std::exception
{
public:
    explicit Error(Diagnostic d);

    // A human-readable, multi-line report for stderr / logs.
    const std::string& report() const
    {
        return report_;
    }

    // The structured diagnostic, for machine handling.
    const Diagnostic& diagnostic() const
    {
        return diag_;
    }

    const char* what() const noexcept override
    {
        return report_.c_str();
    }

private:
    Diagnostic  diag_;
    std::string report_;
};

// --- inline implementations ----------------------------------------------------

inline std::string render_location(const Location& loc)
{
    // An unnamed source still deserves its line and column. Substituting a
    // placeholder name keeps the output in the universal
    // "<file>:<line>:<col>: <message>" shape that editors and humans already
    // know how to read, instead of emitting a bare ":2:5".
    const std::string file = loc.file.empty() ? "<unknown>" : loc.file;
    if (!loc.has_position()) {
        return file;
    }
    return file + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
}

inline Error::Error(Diagnostic d) : diag_(std::move(d))
{
    // Compose "<severity>: <message>" plus an optional location prefix and
    // any notes, each on its own line.
    //
    // The location is included whenever there is anything to say: a file name,
    // a position, or both. Keying this off the file name alone (as it once did)
    // silently threw away the line and column of every diagnostic raised
    // against an unnamed source — and toml::parse(text) with no source_name is
    // an ordinary call, so a config parse error would report "expected a value"
    // with no hint of where.
    const bool        has_location = !diag_.location.file.empty() || diag_.location.has_position();
    const std::string loc          = has_location ? render_location(diag_.location) + ": " : "";

    const char* label = diag_.severity == Severity::Error     ? "error"
                        : diag_.severity == Severity::Warning ? "warning"
                                                              : "note";
    report_.reserve(64 + loc.size() + diag_.message.size());
    report_ += "kap: " + std::string(label) + ": " + loc + diag_.message + "\n";
    for (const std::string& note : diag_.notes) {
        report_ += "      note: " + note + "\n";
    }
}

} // namespace diag
} // namespace kap