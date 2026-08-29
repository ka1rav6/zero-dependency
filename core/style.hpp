#pragma once

// core/style.hpp
//
// Terminal styling for kap's own output (design doc §7's "kap explains itself").
//
// ## Why this is not in exec.cpp
//
// The executor already colours the things it prints — step labels, the dry-run
// gutter — and decides with `exec::default_color()`, which tests **stdout**.
// That is the right question there. Diagnostics go to **stderr**, which is a
// different file descriptor with a different answer: `kap build > log` should
// still colour the error you are about to read on the terminal, and
// `kap build 2> log` should not.
//
// ## Why a palette struct rather than `if (color)` at every call site
//
// Each escape becomes an empty string when styling is off, so a format string
// reads the same either way and there is no second, uncoloured copy of the
// output to keep in sync. Getting that wrong is how a `--no-color` path drifts
// into printing something subtly different from the coloured one.

#include <cstdlib>
#include <unistd.h>

namespace kap
{
namespace style
{

// Every escape kap uses. Deliberately small: four colours and two attributes
// are enough for a diagnostic, and a wider palette would tempt output that
// depends on colour to be readable at all — which it never may, because
// NO_COLOR is always one environment variable away.
struct Palette
{
    const char* reset  = "";
    const char* bold   = "";
    const char* dim    = "";
    const char* red    = "";
    const char* green  = "";
    const char* yellow = "";
    const char* cyan   = "";
};

inline Palette enabled_palette()
{
    Palette p;
    p.reset  = "\033[0m";
    p.bold   = "\033[1m";
    p.dim    = "\033[2m";
    p.red    = "\033[31m";
    p.green  = "\033[32m";
    p.yellow = "\033[33m";
    p.cyan   = "\033[36m";
    return p;
}

// Two gates, the same pair `exec::default_color()` uses and for the same
// reasons — NO_COLOR (no-color.org: any value counts, including empty) answers
// "does this user want colour", isatty answers "would anyone see it" — but
// asked of stderr, where diagnostics actually go.
inline bool stderr_is_styled()
{
    if (std::getenv("NO_COLOR") != nullptr)
        return false;
    return ::isatty(STDERR_FILENO) == 1;
}

inline Palette for_stderr()
{
    return stderr_is_styled() ? enabled_palette() : Palette{};
}

// Unicode is a separate question from colour: a terminal that refuses escape
// sequences may still render UTF-8, and a C-locale terminal may do the
// opposite. Checking the locale environment directly is the only portable
// answer, and the fallbacks below are chosen to line up in the same columns.
inline bool unicode_ok()
{
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            continue;
        for (const char* c = value; *c != '\0'; ++c)
            if ((c[0] == 'U' || c[0] == 'u') && (c[1] == 'T' || c[1] == 't') &&
                (c[2] == 'F' || c[2] == 'f'))
                return true;
        return false;
    }
    return false;
}

inline const char* mark_yes()
{
    return unicode_ok() ? "✓" : "+";
}

inline const char* mark_no()
{
    return unicode_ok() ? "✗" : "x";
}

inline const char* mark_dot()
{
    return unicode_ok() ? "·" : "-";
}

} // namespace style
} // namespace kap
