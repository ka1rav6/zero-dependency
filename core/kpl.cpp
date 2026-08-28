// core/kpl.cpp
//
// KPL lexer. It is deliberately independent of the AST parser: tokenization
// errors can be tested and reported without constructing a plugin.

#include "core/kpl.hpp"

#include <cctype>
#include <charconv>
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
    bool at_end() const
    {
        return position_ >= source_.size();
    }

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
        const int  token_line   = line_;
        const int  token_column = column_;
        const char current      = peek();

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
            case '{':
                add(TokenKind::LeftBrace, 1);
                return;
            case '}':
                add(TokenKind::RightBrace, 1);
                return;
            case '[':
                add(TokenKind::LeftBracket, 1);
                return;
            case ']':
                add(TokenKind::RightBracket, 1);
                return;
            case '(':
                add(TokenKind::LeftParen, 1);
                return;
            case ')':
                add(TokenKind::RightParen, 1);
                return;
            case ':':
                add(TokenKind::Colon, 1);
                return;
            case ',':
                add(TokenKind::Comma, 1);
                return;
            case '.':
                add(TokenKind::Dot, 1);
                return;
            case '+':
                add(TokenKind::Plus, 1);
                return;
            case '*':
                add(TokenKind::Star, 1);
                return;
            case '/':
                add(TokenKind::Slash, 1);
                return;
            case '!':
                add(peek(1) == '=' ? TokenKind::BangEqual : TokenKind::Bang,
                    peek(1) == '=' ? 2 : 1);
                return;
            case '=':
                add(peek(1) == '=' ? TokenKind::EqualEqual
                                   : (peek(1) == '>' ? TokenKind::Arrow : TokenKind::Equal),
                    peek(1) == '=' || peek(1) == '>' ? 2 : 1);
                return;
            case '<':
                add(peek(1) == '=' ? TokenKind::LessEqual : TokenKind::Less,
                    peek(1) == '=' ? 2 : 1);
                return;
            case '>':
                add(peek(1) == '=' ? TokenKind::GreaterEqual : TokenKind::Greater,
                    peek(1) == '=' ? 2 : 1);
                return;
            case '&':
                if (peek(1) == '&') {
                    add(TokenKind::AndAnd, 2);
                    return;
                }
                break;
            case '|':
                if (peek(1) == '|') {
                    add(TokenKind::OrOr, 2);
                    return;
                }
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
        std::int64_t      value  = 0;
        const auto        result = std::from_chars(text.data(), text.data() + text.size(), value);
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
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    fail("unknown string escape", line_, column_ - 1);
            }
        }
        if (at_end()) {
            fail("unterminated string", token_line, token_column);
        }
        advance();
        tokens_.push_back(Token{TokenKind::String, std::move(value), 0, token_line, token_column});
    }

    std::string_view   source_;
    std::string        source_name_;
    std::size_t        position_ = 0;
    int                line_     = 1;
    int                column_   = 1;
    std::vector<Token> tokens_;
};

} // namespace

std::vector<Token> lex(std::string_view source, std::string source_name)
{
    return Lexer{source, std::move(source_name)}.run();
}

namespace
{

class Parser
{
public:
    Parser(std::vector<Token> tokens, std::string source_name)
        : tokens_(std::move(tokens)), source_name_(std::move(source_name))
    {}

    Plugin run()
    {
        Plugin plugin;
        while (!at_end()) {
            const Token keyword =
                consume(TokenKind::Identifier, "expected a top-level declaration");
            if (keyword.text == "manifest")
                plugin.manifest = block();
            else if (keyword.text == "detect")
                plugin.detect = block();
            else if (keyword.text == "requires")
                plugin.requires_block = block();
            else if (keyword.text == "schema")
                plugin.schema = block();
            else if (keyword.text == "command")
                plugin.commands.push_back(command(keyword));
            else
                fail("unknown top-level declaration '" + keyword.text + "'", keyword);
        }
        return plugin;
    }

private:
    bool at_end() const
    {
        return peek().kind == TokenKind::End;
    }

    const Token& peek(std::size_t offset = 0) const
    {
        return tokens_[position_ + offset];
    }

    bool check(TokenKind kind) const
    {
        return peek().kind == kind;
    }

    bool check_text(std::string_view text) const
    {
        return check(TokenKind::Identifier) && peek().text == text;
    }

    bool match(TokenKind kind)
    {
        if (!check(kind))
            return false;
        ++position_;
        return true;
    }

    bool match_text(std::string_view text)
    {
        if (!check_text(text))
            return false;
        ++position_;
        return true;
    }

    Token consume(TokenKind kind, const std::string& message)
    {
        if (!check(kind))
            fail(message, peek());
        return tokens_[position_++];
    }

    Token consume_text(const std::string& text)
    {
        if (!check_text(text))
            fail("expected '" + text + "'", peek());
        return tokens_[position_++];
    }

    [[noreturn]] void fail(const std::string& message, const Token& token) const
    {
        throw diag::Error{
            diag::error(message, diag::Location{source_name_, token.line, token.column})};
    }

    Block block()
    {
        consume(TokenKind::LeftBrace, "expected '{'");
        Block result;
        while (!check(TokenKind::RightBrace) && !at_end())
            result.statements.push_back(statement());
        consume(TokenKind::RightBrace, "expected '}'");
        return result;
    }

    Command command(const Token& token)
    {
        Command result;
        result.token = token;
        result.name  = consume(TokenKind::Identifier, "expected command name").text;
        consume(TokenKind::LeftParen, "expected '(' after command name");
        if (!check(TokenKind::RightParen)) {
            do
                result.parameters.push_back(
                    consume(TokenKind::Identifier, "expected parameter name").text);
            while (match(TokenKind::Comma));
        }
        consume(TokenKind::RightParen, "expected ')' after parameters");
        result.body = block();
        return result;
    }

    Statement statement()
    {
        if (match_text("let")) {
            Statement result;
            result.kind  = Statement::Kind::Let;
            result.token = tokens_[position_ - 1];
            result.name  = consume(TokenKind::Identifier, "expected name after 'let'").text;
            consume(TokenKind::Equal, "expected '=' after name");
            result.expressions.push_back(expression());
            return result;
        }
        if (match_text("step")) {
            Statement result;
            result.kind  = Statement::Kind::Step;
            result.token = tokens_[position_ - 1];
            result.expressions.push_back(expression());
            while (check(TokenKind::String))
                result.expressions.push_back(expression());
            return result;
        }
        if (match_text("if")) {
            Statement result;
            result.kind  = Statement::Kind::If;
            result.token = tokens_[position_ - 1];
            result.expressions.push_back(expression());
            result.body = block().statements;
            if (match_text("else"))
                result.otherwise =
                    match_text("if") ? std::vector<Statement>{statement()} : block().statements;
            return result;
        }
        if (match_text("for")) {
            Statement result;
            result.kind  = Statement::Kind::For;
            result.token = tokens_[position_ - 1];
            result.name  = consume(TokenKind::Identifier, "expected loop variable").text;
            consume_text("in");
            result.expressions.push_back(expression());
            result.body = block().statements;
            return result;
        }
        if (match_text("match")) {
            Statement result;
            result.kind  = Statement::Kind::Match;
            result.token = tokens_[position_ - 1];
            result.expressions.push_back(expression());
            consume(TokenKind::LeftBrace, "expected '{' after match expression");
            while (!check(TokenKind::RightBrace) && !at_end()) {
                result.expressions.push_back(expression());
                consume(TokenKind::Arrow, "expected '=>' after match pattern");
                result.expressions.push_back(expression());
                consume(TokenKind::Comma, "expected ',' after match arm");
            }
            consume(TokenKind::RightBrace, "expected '}' after match arms");
            return result;
        }
        if (match_text("concurrent")) {
            Statement result;
            result.kind  = Statement::Kind::Concurrent;
            result.token = tokens_[position_ - 1];
            result.expressions.push_back(expression());
            return result;
        }
        if (match_text("report_freed_space")) {
            Statement result;
            result.kind  = Statement::Kind::ReportFreedSpace;
            result.token = tokens_[position_ - 1];
            return result;
        }

        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Equal) {
            Statement result;
            result.kind  = Statement::Kind::Assignment;
            result.token = peek();
            result.name  = consume(TokenKind::Identifier, "expected field name").text;
            consume(TokenKind::Equal, "expected '=' after field name");
            result.expressions.push_back(expression());
            return result;
        }
        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
            Statement result;
            result.kind  = Statement::Kind::Assignment;
            result.token = peek();
            result.name  = consume(TokenKind::Identifier, "expected field name").text;
            consume(TokenKind::Colon, "expected ':' after field name");
            if (check_text("enum") || check_text("list")) {
                Expr type;
                type.kind  = Expr::Kind::Name;
                type.token = tokens_[position_++];
                result.expressions.push_back(std::move(type));
            } else {
                result.expressions.push_back(expression());
            }
            if (result.expressions.back().token.text == "enum" && match(TokenKind::LeftBrace)) {
                while (!check(TokenKind::RightBrace) && !at_end()) {
                    result.names.push_back(
                        consume(TokenKind::Identifier, "expected enum member").text);
                    if (!match(TokenKind::Comma))
                        break;
                }
                consume(TokenKind::RightBrace, "expected '}' after enum members");
            } else if (result.expressions.back().token.text == "list" && match(TokenKind::Less)) {
                consume(TokenKind::Identifier, "expected list element type");
                consume(TokenKind::Greater, "expected '>' after list element type");
            }
            if (match(TokenKind::Equal))
                result.expressions.push_back(expression());
            return result;
        }
        Statement result;
        result.kind  = Statement::Kind::Expression;
        result.token = peek();
        result.expressions.push_back(expression());
        return result;
    }

    Expr expression()
    {
        return conditional();
    }

    Expr conditional()
    {
        Expr result = binary(1);
        if (match_text("then")) {
            Expr chosen = expression();
            consume_text("else");
            Expr fallback   = expression();
            Expr condition  = std::move(result);
            result.kind     = Expr::Kind::Conditional;
            result.children = {std::move(condition), std::move(chosen), std::move(fallback)};
        }
        return result;
    }

    Expr binary(int minimum_precedence)
    {
        Expr left = unary();
        while (true) {
            const int precedence = binary_precedence(peek().kind);
            if (precedence < minimum_precedence)
                break;
            const Token op    = tokens_[position_++];
            Expr        right = binary(precedence + 1);
            Expr        result;
            result.kind     = Expr::Kind::Binary;
            result.token    = op;
            result.children = {std::move(left), std::move(right)};
            left            = std::move(result);
        }
        return left;
    }

    static int binary_precedence(TokenKind kind)
    {
        switch (kind) {
            case TokenKind::OrOr:
                return 1;
            case TokenKind::AndAnd:
                return 2;
            case TokenKind::EqualEqual:
            case TokenKind::BangEqual:
                return 3;
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                return 4;
            case TokenKind::Plus:
            case TokenKind::Minus:
                return 5;
            case TokenKind::Star:
            case TokenKind::Slash:
                return 6;
            default:
                return 0;
        }
    }

    Expr unary()
    {
        if (check(TokenKind::Bang) || check(TokenKind::Minus)) {
            Expr result;
            result.kind  = Expr::Kind::Unary;
            result.token = tokens_[position_++];
            result.children.push_back(unary());
            return result;
        }
        return postfix();
    }

    Expr postfix()
    {
        Expr result = primary();
        while (true) {
            if (match(TokenKind::Dot)) {
                Expr next;
                next.kind  = Expr::Kind::Member;
                next.token = consume(TokenKind::Identifier, "expected member name");
                next.children.push_back(std::move(result));
                result = std::move(next);
            } else if (match(TokenKind::LeftParen)) {
                Expr next;
                next.kind = Expr::Kind::Call;
                next.children.push_back(std::move(result));
                if (!check(TokenKind::RightParen)) {
                    do
                        next.children.push_back(expression());
                    while (match(TokenKind::Comma));
                }
                consume(TokenKind::RightParen, "expected ')' after arguments");
                result = std::move(next);
            } else if (match(TokenKind::LeftBracket)) {
                Expr next;
                next.kind     = Expr::Kind::Index;
                next.children = {std::move(result), expression()};
                consume(TokenKind::RightBracket, "expected ']' after index");
                result = std::move(next);
            } else
                break;
        }
        return result;
    }

    Expr primary()
    {
        const Token token = peek();
        if (match_text("if")) {
            Expr result;
            result.kind  = Expr::Kind::Conditional;
            result.token = token;
            result.children.push_back(binary(1));
            consume_text("then");
            result.children.push_back(expression());
            consume_text("else");
            result.children.push_back(expression());
            return result;
        }
        if (match(TokenKind::String))
            return Expr{Expr::Kind::String, token, {}, {}};
        if (match(TokenKind::Integer))
            return Expr{Expr::Kind::Integer, token, {}, {}};
        if (check_text("true") || check_text("false")) {
            ++position_;
            return Expr{Expr::Kind::Boolean, token, {}, {}};
        }
        if (match_text("none"))
            return Expr{Expr::Kind::None, token, {}, {}};
        if (match(TokenKind::Identifier))
            return Expr{Expr::Kind::Name, token, {}, {}};
        if (match(TokenKind::LeftParen)) {
            Expr result = expression();
            consume(TokenKind::RightParen, "expected ')' after expression");
            return result;
        }
        if (match(TokenKind::LeftBracket)) {
            Expr result;
            result.kind = Expr::Kind::List;
            if (!check(TokenKind::RightBracket)) {
                do
                    result.children.push_back(expression());
                while (match(TokenKind::Comma));
            }
            consume(TokenKind::RightBracket, "expected ']' after list");
            return result;
        }
        if (match(TokenKind::LeftBrace)) {
            Expr result;
            result.kind = Expr::Kind::Record;
            while (!check(TokenKind::RightBrace) && !at_end()) {
                result.names.push_back(
                    consume(TokenKind::Identifier, "expected record field").text);
                consume(TokenKind::Colon, "expected ':' after record field");
                result.children.push_back(expression());
                if (!match(TokenKind::Comma))
                    break;
            }
            consume(TokenKind::RightBrace, "expected '}' after record");
            return result;
        }
        fail("expected an expression", token);
    }

    std::vector<Token> tokens_;
    std::string        source_name_;
    std::size_t        position_ = 0;
};

} // namespace

Plugin parse(std::string_view source, std::string source_name)
{
    std::vector<Token> tokens = lex(source, source_name);
    return Parser{std::move(tokens), std::move(source_name)}.run();
}

} // namespace kap::kpl
