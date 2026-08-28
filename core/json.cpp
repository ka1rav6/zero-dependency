// core/json.cpp
//
// Implementation of the minimal JSON subset declared in core/json.hpp. A
// hand-written recursive-descent parser over a string_view, in the same shape
// as core/toml.cpp and the KPL lexer, so a reader who has understood one has
// understood all three.

#include "core/json.hpp"

#include <cctype>
#include <charconv>
#include <string>
#include <utility>

#include "core/diag.hpp"

namespace kap
{
namespace json
{

const Value* Value::find(std::string_view key) const
{
    if (kind != Kind::Object) {
        return nullptr;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

Value make_null()
{
    return Value{};
}

Value make_boolean(bool value)
{
    Value result;
    result.kind    = Value::Kind::Boolean;
    result.boolean = value;
    return result;
}

Value make_integer(std::int64_t value)
{
    Value result;
    result.kind    = Value::Kind::Integer;
    result.integer = value;
    return result;
}

Value make_string(std::string value)
{
    Value result;
    result.kind   = Value::Kind::String;
    result.string = std::move(value);
    return result;
}

Value make_array(std::vector<Value> items)
{
    Value result;
    result.kind  = Value::Kind::Array;
    result.array = std::move(items);
    return result;
}

Value make_object(std::map<std::string, Value> fields)
{
    Value result;
    result.kind   = Value::Kind::Object;
    result.object = std::move(fields);
    return result;
}

namespace
{

// Recursion guard.
//
// JSON nests arbitrarily deep and this parser recurses per level, so a file
// consisting of ten thousand '[' characters would blow the stack before any
// syntax error was reported. Golden files and cache entries are two or three
// levels deep; a cap two orders of magnitude above that costs nothing real and
// turns a crash into a diagnostic. Design doc §7 treats plugin-adjacent input
// as untrusted, and a plugin's tests/ directory is exactly that.
constexpr int kMaxDepth = 64;

class Parser
{
public:
    Parser(std::string_view text, std::string source_name)
        : text_(text), source_name_(std::move(source_name))
    {}

    Value run()
    {
        skip_whitespace();
        Value result = value(0);
        skip_whitespace();
        if (!at_end()) {
            fail("unexpected trailing content after the top-level value");
        }
        return result;
    }

private:
    bool at_end() const
    {
        return position_ >= text_.size();
    }

    char peek() const
    {
        return at_end() ? '\0' : text_[position_];
    }

    char advance()
    {
        const char current = peek();
        if (current != '\0') {
            ++position_;
            if (current == '\n') {
                ++line_;
                column_ = 1;
            } else {
                ++column_;
            }
        }
        return current;
    }

    [[noreturn]] void fail(const std::string& message) const
    {
        throw diag::Error{diag::error(message, diag::Location{source_name_, line_, column_})};
    }

    void skip_whitespace()
    {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') {
            advance();
        }
    }

    // Consume `word` if it is next; used for the three literals.
    bool match_word(std::string_view word)
    {
        if (text_.substr(position_, word.size()) != word) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            advance();
        }
        return true;
    }

    Value value(int depth)
    {
        if (depth > kMaxDepth) {
            fail("JSON nesting is deeper than " + std::to_string(kMaxDepth) + " levels");
        }
        skip_whitespace();
        switch (peek()) {
            case '{':
                return object(depth);
            case '[':
                return array(depth);
            case '"':
                return make_string(string());
            case 't':
                if (match_word("true")) {
                    return make_boolean(true);
                }
                break;
            case 'f':
                if (match_word("false")) {
                    return make_boolean(false);
                }
                break;
            case 'n':
                if (match_word("null")) {
                    return make_null();
                }
                break;
            default:
                break;
        }
        if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
            return integer();
        }
        fail("expected a JSON value");
    }

    Value object(int depth)
    {
        advance(); // '{'
        Value result = make_object();
        skip_whitespace();
        if (peek() == '}') {
            advance();
            return result;
        }
        for (;;) {
            skip_whitespace();
            if (peek() != '"') {
                fail("expected a quoted object key");
            }
            std::string key = string();
            skip_whitespace();
            if (advance() != ':') {
                fail("expected ':' after an object key");
            }
            Value entry = value(depth + 1);
            // A duplicate key has no single right answer (last-wins is a
            // convention, not a rule), and in a golden file it always means a
            // mistake. Report it instead of picking one.
            if (!result.object.emplace(std::move(key), std::move(entry)).second) {
                fail("duplicate object key");
            }
            skip_whitespace();
            const char separator = advance();
            if (separator == '}') {
                return result;
            }
            if (separator != ',') {
                fail("expected ',' or '}' in an object");
            }
        }
    }

    Value array(int depth)
    {
        advance(); // '['
        Value result = make_array();
        skip_whitespace();
        if (peek() == ']') {
            advance();
            return result;
        }
        for (;;) {
            result.array.push_back(value(depth + 1));
            skip_whitespace();
            const char separator = advance();
            if (separator == ']') {
                return result;
            }
            if (separator != ',') {
                fail("expected ',' or ']' in an array");
            }
        }
    }

    std::string string()
    {
        advance(); // opening quote
        std::string out;
        for (;;) {
            if (at_end()) {
                fail("unterminated string");
            }
            const char current = advance();
            if (current == '"') {
                return out;
            }
            // Raw control characters are invalid in JSON strings, and letting
            // a stray newline through would make the reported location of any
            // later error wrong.
            if (static_cast<unsigned char>(current) < 0x20) {
                fail("unescaped control character in a string");
            }
            if (current != '\\') {
                out.push_back(current);
                continue;
            }
            switch (advance()) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    fail("\\uXXXX escapes are not supported; write the character directly");
                default:
                    fail("unknown string escape");
            }
        }
    }

    Value integer()
    {
        const std::size_t start = position_;
        if (peek() == '-') {
            advance();
        }
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            fail("expected a digit");
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        // Reject rather than truncate: see the header for why floats are out.
        if (peek() == '.' || peek() == 'e' || peek() == 'E') {
            fail("floating-point numbers are not supported");
        }

        const std::string_view digits = text_.substr(start, position_ - start);
        std::int64_t           parsed = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
            fail("integer is out of range for a 64-bit signed value");
        }
        return make_integer(parsed);
    }

    std::string_view text_;
    std::string      source_name_;
    std::size_t      position_ = 0;
    int              line_     = 1;
    int              column_   = 1;
};

void write_string(const std::string& value, std::string& out)
{
    out += '"';
    for (const char character : value) {
        switch (character) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Remaining control characters have no short escape and
                // \uXXXX is not in this subset, so they are dropped rather
                // than emitted raw — raw would produce a file this parser
                // itself refuses to read back.
                if (static_cast<unsigned char>(character) >= 0x20) {
                    out += character;
                }
                break;
        }
    }
    out += '"';
}

void write_value(const Value& value, bool pretty, int indent, std::string& out)
{
    const std::string pad = pretty ? std::string(static_cast<std::size_t>(indent) * 2, ' ') : "";
    const std::string pad_in =
        pretty ? std::string(static_cast<std::size_t>(indent + 1) * 2, ' ') : "";
    const char* newline = pretty ? "\n" : "";
    const char* colon   = pretty ? ": " : ":";

    switch (value.kind) {
        case Value::Kind::Null:
            out += "null";
            return;
        case Value::Kind::Boolean:
            out += value.boolean ? "true" : "false";
            return;
        case Value::Kind::Integer:
            out += std::to_string(value.integer);
            return;
        case Value::Kind::String:
            write_string(value.string, out);
            return;
        case Value::Kind::Array:
            if (value.array.empty()) {
                out += "[]";
                return;
            }
            out += '[';
            out += newline;
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                out += pad_in;
                write_value(value.array[i], pretty, indent + 1, out);
                if (i + 1 != value.array.size()) {
                    out += ',';
                }
                out += newline;
            }
            out += pad;
            out += ']';
            return;
        case Value::Kind::Object:
            if (value.object.empty()) {
                out += "{}";
                return;
            }
            out += '{';
            out += newline;
            {
                std::size_t index = 0;
                for (const auto& [key, entry] : value.object) {
                    out += pad_in;
                    write_string(key, out);
                    out += colon;
                    write_value(entry, pretty, indent + 1, out);
                    if (++index != value.object.size()) {
                        out += ',';
                    }
                    out += newline;
                }
            }
            out += pad;
            out += '}';
            return;
    }
}

} // namespace

Value parse(std::string_view text, std::string source_name)
{
    return Parser{text, std::move(source_name)}.run();
}

std::string write(const Value& value, bool pretty)
{
    std::string out;
    write_value(value, pretty, 0, out);
    if (pretty) {
        // A trailing newline makes the output a well-formed text file, which
        // matters because these are committed as golden files.
        out += '\n';
    }
    return out;
}

} // namespace json
} // namespace kap
