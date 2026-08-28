// core/kpl.cpp
//
// KPL lexer. It is deliberately independent of the AST parser: tokenization
// errors can be tested and reported without constructing a plugin.

#include "core/kpl.hpp"

#include <charconv>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

#include "core/diag.hpp"

namespace kap::kpl
{
namespace
{

class Lexer
{
public:
    Lexer(std::string_view source, std::string source_name)
        : source_(source), source_name_(std::move(source_name))
    {}

    std::vector<Token> run()
    {
        while (!at_end()) {
            skip_space_and_comments();
            if (at_end()) {
                break;
            }
            scan_token();
        }
        tokens_.push_back(Token{TokenKind::End, {}, 0, line_, column_});
        return tokens_;
    }

private:
    bool at_end() const { return position_ >= source_.size(); }

    char peek(std::size_t offset = 0) const
    {
        const std::size_t index = position_ + offset;
        return index < source_.size() ? source_[index] : '\0';
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

    [[noreturn]] void fail(const std::string& message, int line, int column) const
    {
        throw diag::Error{diag::error(message, diag::Location{source_name_, line, column})};
    }

    void skip_space_and_comments()
    {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') {
                advance();
            }
            if (peek() != '/' || peek(1) != '/') {
                return;
            }
            while (!at_end() && peek() != '\n') {
                advance();
            }
        }
    }

    void scan_token()
    {
        const int token_line = line_;
        const int token_column = column_;
        const char current = peek();

        if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
            scan_identifier(token_line, token_column);
            return;
        }
        if (std::isdigit(static_cast<unsigned char>(current))) {
            scan_integer(token_line, token_column);
            return;
        }
        if (current == '"') {
            scan_string(token_line, token_column);
            return;
        }

        const auto add = [this, token_line, token_column](TokenKind kind, std::size_t length) {
            std::string text;
            text.reserve(length);
            for (std::size_t i = 0; i < length; ++i) {
                text.push_back(advance());
            }
            tokens_.push_back(Token{kind, std::move(text), 0, token_line, token_column});
        };

        switch (current) {
        case '{': add(TokenKind::LeftBrace, 1); return;
        case '}': add(TokenKind::RightBrace, 1); return;
        case '[': add(TokenKind::LeftBracket, 1); return;
        case ']': add(TokenKind::RightBracket, 1); return;
        case '(': add(TokenKind::LeftParen, 1); return;
        case ')': add(TokenKind::RightParen, 1); return;
        case ':': add(TokenKind::Colon, 1); return;
        case ',': add(TokenKind::Comma, 1); return;
        case '.': add(TokenKind::Dot, 1); return;
        case '+': add(TokenKind::Plus, 1); return;
        case '*': add(TokenKind::Star, 1); return;
        case '/': add(TokenKind::Slash, 1); return;
        case '!': add(peek(1) == '=' ? TokenKind::BangEqual : TokenKind::Bang,
                      peek(1) == '=' ? 2 : 1); return;
        case '=': add(peek(1) == '=' ? TokenKind::EqualEqual :
                       (peek(1) == '>' ? TokenKind::Arrow : TokenKind::Equal),
                      peek(1) == '=' || peek(1) == '>' ? 2 : 1); return;
        case '<': add(peek(1) == '=' ? TokenKind::LessEqual : TokenKind::Less,
                      peek(1) == '=' ? 2 : 1); return;
        case '>': add(peek(1) == '=' ? TokenKind::GreaterEqual : TokenKind::Greater,
                      peek(1) == '=' ? 2 : 1); return;
        case '&':
            if (peek(1) == '&') { add(TokenKind::AndAnd, 2); return; }
            break;
        case '|':
            if (peek(1) == '|') { add(TokenKind::OrOr, 2); return; }
            break;
        case '-':
            add(TokenKind::Minus, 1);
            return;
        default:
            break;
        }
        fail("unexpected character '" + std::string(1, current) + "'", token_line, token_column);
    }

    void scan_identifier(int token_line, int token_column)
    {
        const std::size_t start = position_;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
            advance();
        }
        tokens_.push_back(Token{TokenKind::Identifier,
                                std::string(source_.substr(start, position_ - start)),
                                0,
                                token_line,
                                token_column});
    }

    void scan_integer(int token_line, int token_column)
    {
        const std::size_t start = position_;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        const std::string text(source_.substr(start, position_ - start));
        std::int64_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            fail("integer literal is out of range", token_line, token_column);
        }
        tokens_.push_back(Token{TokenKind::Integer, text, value, token_line, token_column});
    }

    void scan_string(int token_line, int token_column)
    {
        advance();
        std::string value;
        while (!at_end() && peek() != '"') {
            if (peek() == '\n' || peek() == '\r') {
                fail("unterminated string", token_line, token_column);
            }
            if (advance() != '\\') {
                value.push_back(source_[position_ - 1]);
                continue;
            }
            const char escaped = peek();
            advance();
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            default: fail("unknown string escape", line_, column_ - 1);
            }
        }
        if (at_end()) {
            fail("unterminated string", token_line, token_column);
        }
        advance();
        tokens_.push_back(Token{TokenKind::String, std::move(value), 0, token_line, token_column});
    }

    std::string_view source_;
    std::string source_name_;
    std::size_t position_ = 0;
    int line_ = 1;
    int column_ = 1;
    std::vector<Token> tokens_;
};

} // namespace

std::vector<Token> lex(std::string_view source, std::string source_name)
{
    return Lexer{source, std::move(source_name)}.run();
}

} // namespace kap::kpl
