#pragma once

// core/argv.hpp
//
// The argv array used by KPL `step` statements, and its display rendering
// (design doc Milestone 1, and §5.4 where a CommandSpec carries argv arrays).
//
// Why a dedicated type instead of a plain std::vector<std::string>? Two
// reasons:
//   1. Executor code can tell "this is an argv array" apart from arbitrary
//      string lists, which keeps the step-spec contract unambiguous.
//   2. `--dry-run` and error messages need every word shell-quoted for
//      display (design doc §7: "always show the exact argv arrays"). The
//      quoting rules live here, in one place, so a dry-run always shows the
//      user exactly what would run.

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace argv
{

// Escape a single word for display in a shell command line. Words that are
// already safe (alphanumerics and a small whitelist) pass through untouched so
// the output stays readable; anything else is single-quoted with embedded
// single quotes escaped per POSIX shell rules.
std::string escape_word(std::string_view word);

// An ordered list of program arguments.
class List
{
public:
    List() = default;

    // Build from an initializer list so plugins read naturally:
    //   kap::argv::List cmd = {"cmake", "--build", "build"};
    List(std::initializer_list<std::string> items);
    explicit List(std::vector<std::string> items);

    bool empty() const
    {
        return items_.empty();
    }

    std::size_t size() const
    {
        return items_.size();
    }

    void push_back(std::string item)
    {
        items_.push_back(std::move(item));
    }

    void append(std::initializer_list<std::string> items);
    void append(const std::vector<std::string>& items);

    const std::string& operator[](std::size_t index) const
    {
        return items_.at(index);
    }

    // Raw access; most callers only need the const view.
    const std::vector<std::string>& vec() const
    {
        return items_;
    }

    // Space-joined, unquoted (used for logs and simple messages).
    std::string joined(std::string_view sep = " ") const;

    // The display form: each word shell-quoted, inserted verbatim into a
    // quoted string. Example: `cmake --build 'my dir'`.
    std::string quoted() const;

private:
    std::vector<std::string> items_;
};

// --- inline implementations ----------------------------------------------------

inline List::List(std::initializer_list<std::string> items) : items_(items) {}

inline List::List(std::vector<std::string> items) : items_(std::move(items)) {}

inline void List::append(std::initializer_list<std::string> items)
{
    items_.insert(items_.end(), items.begin(), items.end());
}

inline void List::append(const std::vector<std::string>& items)
{
    items_.insert(items_.end(), items.begin(), items.end());
}

inline std::string List::joined(std::string_view sep) const
{
    std::string out;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i != 0) {
            out += sep;
        }
        out += items_[i];
    }
    return out;
}

inline std::string List::quoted() const
{
    std::string out;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i != 0) {
            out += ' ';
        }
        out += escape_word(items_[i]);
    }
    return out;
}

inline std::string escape_word(std::string_view word)
{
    // Allow-list of characters that are safe inside a shell command word. Any
    // character outside this set forces full quoting.
    constexpr std::string_view kSafe =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-=+/.,:@%^";

    // Fast path: if every character is "safe", show the word verbatim so
    // dry-runs stay readable.
    bool needs_quoting = word.empty();
    for (const char c : word) {
        if (kSafe.find(c) == std::string_view::npos) {
            needs_quoting = true;
            break;
        }
    }
    if (!needs_quoting) {
        return std::string(word);
    }

    // POSIX single-quote rule: 'word' yields the literal word; an embedded '
    // is written '\'' — close the quote, escape the quote, reopen it.
    std::string out = "'";
    for (const char ch : word) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

} // namespace argv
} // namespace kap