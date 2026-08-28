// core/kpl.cpp
//
// KPL lexer. It is deliberately independent of the AST parser: tokenization
// errors can be tested and reported without constructing a plugin.

#include "core/kpl.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include <unistd.h>

#include "core/diag.hpp"
#include "core/fs.hpp"

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
                plugin.detect = directive_block();
            else if (keyword.text == "requires")
                plugin.requires_block = directive_block();
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

    // `detect` and `requires` hold *directives*, not statements.
    //
    // The generic statement parser gets both blocks wrong. `file_exists
    // "CMakeLists.txt"` parses as two unrelated statements — a bare name and a
    // stray string — so a Milestone-4 rule extractor would have nothing to
    // read. Worse, `optional [ninja, make, ccache]` is a hard parse error,
    // because a '[' after an identifier is an index expression and an index
    // takes exactly one subscript. The design doc writes both forms (§3.2,
    // §5.3, §5.10), so neither is optional.
    //
    // A directive is `IDENT` followed by zero or more arguments, where an
    // argument is a literal, a list, or a record — never a bare identifier.
    // That restriction is what makes the block unambiguous: an identifier at
    // argument position always starts the next directive. Bare identifiers
    // inside a list are read as strings, so `any_of [cmake]` means the tool
    // named "cmake" without demanding quotes the design doc does not write.
    Block directive_block()
    {
        consume(TokenKind::LeftBrace, "expected '{'");
        Block result;
        while (!check(TokenKind::RightBrace) && !at_end()) {
            Statement directive;
            directive.kind  = Statement::Kind::Directive;
            directive.token = peek();
            directive.name  = consume(TokenKind::Identifier, "expected a directive name").text;
            while (starts_directive_argument())
                directive.expressions.push_back(directive_argument());
            result.statements.push_back(std::move(directive));
        }
        consume(TokenKind::RightBrace, "expected '}'");
        return result;
    }

    bool starts_directive_argument() const
    {
        switch (peek().kind) {
            case TokenKind::String:
            case TokenKind::Integer:
            case TokenKind::LeftBracket:
            case TokenKind::LeftBrace:
                return true;
            default:
                return false;
        }
    }

    Expr directive_argument()
    {
        const Token token = peek();
        if (match(TokenKind::String))
            return Expr{Expr::Kind::String, token, {}, {}};
        if (match(TokenKind::Integer))
            return Expr{Expr::Kind::Integer, token, {}, {}};
        if (match(TokenKind::LeftBracket)) {
            Expr result;
            result.kind  = Expr::Kind::List;
            result.token = token;
            if (!check(TokenKind::RightBracket)) {
                do
                    result.children.push_back(directive_list_element());
                while (match(TokenKind::Comma));
            }
            consume(TokenKind::RightBracket, "expected ']' after a directive list");
            return result;
        }
        // A record argument, e.g.
        //   file_contains { path: "package.json", pattern: "\"workspaces\"" }
        consume(TokenKind::LeftBrace, "expected a directive argument");
        Expr result;
        result.kind  = Expr::Kind::Record;
        result.token = token;
        while (!check(TokenKind::RightBrace) && !at_end()) {
            const Token field = consume(TokenKind::Identifier, "expected a record field");
            if (std::find(result.names.begin(), result.names.end(), field.text) !=
                result.names.end())
                fail("duplicate record field '" + field.text + "'", field);
            result.names.push_back(field.text);
            consume(TokenKind::Colon, "expected ':' after a record field");
            result.children.push_back(directive_list_element());
            if (!match(TokenKind::Comma))
                break;
        }
        consume(TokenKind::RightBrace, "expected '}' after a record argument");
        return result;
    }

    // Elements inside a directive list or record: literals, plus bare
    // identifiers read as their own text.
    Expr directive_list_element()
    {
        const Token token = peek();
        if (match(TokenKind::String))
            return Expr{Expr::Kind::String, token, {}, {}};
        if (match(TokenKind::Integer))
            return Expr{Expr::Kind::Integer, token, {}, {}};
        if (check_text("true") || check_text("false")) {
            ++position_;
            return Expr{Expr::Kind::Boolean, token, {}, {}};
        }
        if (match(TokenKind::Identifier))
            return Expr{Expr::Kind::String, token, {}, {}};
        fail("expected a literal or a name", token);
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
            while (starts_step_argument() && !is_statement_start())
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
        if (check_text("match")) {
            // `match` is an expression (design doc §5.9 assigns one to a
            // `let`), so statement position just parses it as one and keeps
            // the value. Statement::Kind::Match is retained so a reader of the
            // AST can still see that this line is a match.
            Statement result;
            result.kind  = Statement::Kind::Match;
            result.token = peek();
            result.expressions.push_back(expression());
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
                type.kind        = Expr::Kind::Name;
                type.token       = tokens_[position_++];
                result.type_name = type.token.text;
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
                result.type_name +=
                    "<" + consume(TokenKind::Identifier, "expected list element type").text + ">";
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

    bool starts_step_argument() const
    {
        switch (peek().kind) {
            case TokenKind::String:
            case TokenKind::Integer:
            case TokenKind::Identifier:
            case TokenKind::LeftBracket:
            case TokenKind::LeftBrace:
            case TokenKind::LeftParen:
            case TokenKind::Bang:
            case TokenKind::Minus:
                return true;
            default:
                return false;
        }
    }

    bool is_statement_start() const
    {
        return check_text("let") || check_text("step") || check_text("if") || check_text("for") ||
               check_text("match") || check_text("concurrent") ||
               check_text("report_freed_space") || check(TokenKind::RightBrace);
    }

    Expr conditional()
    {
        Expr condition = binary(1);
        if (!check_text("then"))
            return condition;

        // The `then` token is the node's anchor for diagnostics. The previous
        // version reused the `result` object it had just moved *from* as the
        // node, so the Conditional carried a moved-from Token: empty file
        // text, and whatever line/column the moved-from int members happened
        // to keep. Building a fresh node is both clearer and correct.
        const Token keyword = tokens_[position_++];
        Expr        chosen  = expression();
        consume_text("else");
        Expr fallback = expression();

        Expr result;
        result.kind     = Expr::Kind::Conditional;
        result.token    = keyword;
        result.children = {std::move(condition), std::move(chosen), std::move(fallback)};
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
            } else if (check(TokenKind::LeftBracket)) {
                // Anchor on the '[' so an out-of-range index reports where the
                // subscript is. The node used to be built with a
                // default-constructed token, so every such error said 1:1.
                Expr next;
                next.kind     = Expr::Kind::Index;
                next.token    = tokens_[position_++];
                next.children = {std::move(result), expression()};
                consume(TokenKind::RightBracket, "expected ']' after index");
                result = std::move(next);
            } else
                break;
        }
        return result;
    }

    // pattern = literal | IDENT | "none"  (design doc §5.5)
    //
    // A bare IDENT is an enum *member*, not a variable read: §5.3 writes
    // `ninja => "Ninja"` where `ninja` is a value of the `generator` enum and
    // is never bound to anything. It is kept as a Name node and compared as
    // its own text.
    Expr pattern()
    {
        const Token token = peek();
        if (check(TokenKind::String) || check(TokenKind::Integer)) {
            ++position_;
            return Expr{
                check_previous_string() ? Expr::Kind::String : Expr::Kind::Integer, token, {}, {}};
        }
        if (check_text("true") || check_text("false")) {
            ++position_;
            return Expr{Expr::Kind::Boolean, token, {}, {}};
        }
        if (match_text("none"))
            return Expr{Expr::Kind::None, token, {}, {}};
        if (match(TokenKind::Identifier))
            return Expr{Expr::Kind::Name, token, {}, {}};
        fail("expected a match pattern", token);
    }

    bool check_previous_string() const
    {
        return tokens_[position_ - 1].kind == TokenKind::String;
    }

    Expr primary()
    {
        const Token token = peek();
        if (match_text("match")) {
            Expr result;
            result.kind  = Expr::Kind::Match;
            result.token = token;
            // The subject is parsed with binary() rather than expression() so
            // the '{' that opens the arms cannot be mistaken for the start of
            // a record literal, and so a trailing `then` belongs to the match
            // rather than to the subject.
            result.children.push_back(binary(1));
            consume(TokenKind::LeftBrace, "expected '{' after the match subject");
            while (!check(TokenKind::RightBrace) && !at_end()) {
                result.children.push_back(pattern());
                consume(TokenKind::Arrow, "expected '=>' after a match pattern");
                result.children.push_back(expression());
                if (!match(TokenKind::Comma))
                    break;
            }
            consume(TokenKind::RightBrace, "expected '}' after the match arms");
            if (result.children.size() < 3)
                fail("match requires at least one arm", token);
            return result;
        }
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
            result.kind  = Expr::Kind::Record;
            result.token = token;
            while (!check(TokenKind::RightBrace) && !at_end()) {
                const Token field = consume(TokenKind::Identifier, "expected record field");
                // A repeated field has no sensible meaning and always means a
                // mistake. Keeping one of them silently would drop the very
                // value the author was setting — the record form of `step` is
                // where this bites, since a dropped `cwd` runs the command in
                // the wrong directory with nothing to go on.
                if (std::find(result.names.begin(), result.names.end(), field.text) !=
                    result.names.end())
                    fail("duplicate record field '" + field.text + "'", field);
                result.names.push_back(field.text);
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
    Plugin             plugin = Parser{std::move(tokens), source_name}.run();
    plugin.source_name        = std::move(source_name);
    return plugin;
}

Value Value::none()
{
    return Value{};
}

Value Value::string_value(std::string value)
{
    Value result;
    result.kind   = Kind::String;
    result.string = std::move(value);
    return result;
}

Value Value::integer_value(std::int64_t value)
{
    Value result;
    result.kind    = Kind::Integer;
    result.integer = value;
    return result;
}

Value Value::boolean_value(bool value)
{
    Value result;
    result.kind    = Kind::Boolean;
    result.boolean = value;
    return result;
}

Value Value::list_value(std::vector<Value> value)
{
    Value result;
    result.kind = Kind::List;
    result.list = std::move(value);
    return result;
}

Value Value::record_value(std::map<std::string, Value> value)
{
    Value result;
    result.kind   = Kind::Record;
    result.record = std::move(value);
    return result;
}

namespace
{

bool schema_value_matches(const Value&                    value,
                          std::string_view                type,
                          const std::vector<std::string>& enum_values)
{
    if (type == "str")
        return value.kind == Value::Kind::String;
    if (type == "int")
        return value.kind == Value::Kind::Integer;
    if (type == "bool")
        return value.kind == Value::Kind::Boolean;
    if (type == "list<str>" || type == "list<int>") {
        if (value.kind != Value::Kind::List)
            return false;
        const Value::Kind element_kind =
            type == "list<str>" ? Value::Kind::String : Value::Kind::Integer;
        for (const Value& element : value.list)
            if (element.kind != element_kind)
                return false;
        return true;
    }
    if (type == "enum") {
        if (value.kind != Value::Kind::String)
            return false;
        return std::find(enum_values.begin(), enum_values.end(), value.string) != enum_values.end();
    }
    return false;
}

} // namespace

std::vector<SchemaField> schema(const Plugin& plugin)
{
    std::vector<SchemaField> fields;
    if (!plugin.schema)
        return fields;
    for (const Statement& statement : plugin.schema->statements) {
        if (statement.kind != Statement::Kind::Assignment || statement.expressions.empty())
            continue;
        SchemaField field;
        field.name = statement.name;
        field.type = statement.type_name.empty() ? statement.expressions.front().token.text
                                                 : statement.type_name;
        if (field.type == "enum")
            field.enum_values = statement.names;
        if (statement.expressions.size() == 2) {
            const Expr& value = statement.expressions.back();
            switch (value.kind) {
                case Expr::Kind::String:
                    field.default_value = Value::string_value(value.token.text);
                    break;
                case Expr::Kind::Integer:
                    field.default_value = Value::integer_value(value.token.integer);
                    break;
                case Expr::Kind::Boolean:
                    field.default_value = Value::boolean_value(value.token.text == "true");
                    break;
                case Expr::Kind::None:
                    field.default_value = Value::none();
                    break;
                case Expr::Kind::Name:
                    if (field.type == "enum")
                        field.default_value = Value::string_value(value.token.text);
                    break;
                case Expr::Kind::Unary:
                    // `retries: int = -1`. The lexer emits '-' as its own
                    // token, so a negative literal arrives here as a unary
                    // node. Without this case the field looked like it had no
                    // default at all and build_config rejected the plugin with
                    // "requires a default".
                    if (value.token.kind == TokenKind::Minus && value.children.size() == 1 &&
                        value.children.front().kind == Expr::Kind::Integer)
                        field.default_value =
                            Value::integer_value(-value.children.front().token.integer);
                    break;
                case Expr::Kind::List:
                    {
                        std::vector<Value> values;
                        for (const Expr& child : value.children) {
                            if (child.kind == Expr::Kind::String)
                                values.push_back(Value::string_value(child.token.text));
                            else if (child.kind == Expr::Kind::Integer)
                                values.push_back(Value::integer_value(child.token.integer));
                        }
                        if (values.size() == value.children.size())
                            field.default_value = Value::list_value(std::move(values));
                        break;
                    }
                default:
                    break;
            }
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

std::pair<std::map<std::string, Value>, std::vector<std::string>>
build_config(const Plugin& plugin, const std::map<std::string, Value>& overrides)
{
    std::map<std::string, Value>              result;
    std::vector<std::string>                  errors;
    const auto                                fields = schema(plugin);
    std::map<std::string, const SchemaField*> known;
    for (const SchemaField& field : fields) {
        if (!known.emplace(field.name, &field).second) {
            errors.push_back("duplicate schema field '" + field.name + "'");
            continue;
        }
        if (!field.default_value)
            errors.push_back("schema field '" + field.name + "' requires a default");
        else if (!schema_value_matches(*field.default_value, field.type, field.enum_values)) {
            // Say which rule was broken. "has type enum" told the author
            // nothing when the real problem was a default naming a member the
            // enum does not declare.
            if (field.type == "enum" && field.default_value->kind == Value::Kind::String) {
                std::string members;
                for (const std::string& member : field.enum_values)
                    members += (members.empty() ? "" : ", ") + member;
                errors.push_back("default '" + field.default_value->string +
                                 "' for schema field '" + field.name +
                                 "' is not one of its members (" + members + ")");
            } else {
                errors.push_back("default for schema field '" + field.name + "' must be " +
                                 field.type);
            }
        } else
            result[field.name] = *field.default_value;
    }
    for (const auto& [name, value] : overrides) {
        const auto found = known.find(name);
        if (found == known.end()) {
            errors.push_back("unknown config key '" + name + "'");
            continue;
        }
        if (!schema_value_matches(value, found->second->type, found->second->enum_values)) {
            errors.push_back("config key '" + name + "' must be " + found->second->type);
            continue;
        }
        result[name] = value;
    }
    return {std::move(result), std::move(errors)};
}

namespace
{

enum class StaticType
{
    Unknown,
    None,
    String,
    Integer,
    Boolean,
    ListString,
    ListInteger,
    ListUnknown,
    Record
};

const char* type_name(StaticType type)
{
    switch (type) {
        case StaticType::None:
            return "none";
        case StaticType::String:
            return "string";
        case StaticType::Integer:
            return "integer";
        case StaticType::Boolean:
            return "boolean";
        case StaticType::ListString:
            return "list<string>";
        case StaticType::ListInteger:
            return "list<integer>";
        case StaticType::ListUnknown:
            return "list";
        case StaticType::Record:
            return "record";
        case StaticType::Unknown:
            return "unknown";
    }
    return "unknown";
}

// Map a schema field's declared type (design doc §5.7) onto the checker's
// lattice. `enum` members are ordinary strings at run time — the closed set is
// enforced when the config record is built, not when KPL reads it.
StaticType schema_type(const SchemaField& field)
{
    if (field.type == "str" || field.type == "enum")
        return StaticType::String;
    if (field.type == "int")
        return StaticType::Integer;
    if (field.type == "bool")
        return StaticType::Boolean;
    if (field.type == "list<str>")
        return StaticType::ListString;
    if (field.type == "list<int>")
        return StaticType::ListInteger;
    return StaticType::Unknown;
}

class TypeChecker
{
public:
    // `fields` is the plugin's schema block; `has_schema_block` says whether
    // the plugin declared one at all.
    //
    // Knowing the schema is what lets `config.cmake_args` type as list<string>
    // instead of Unknown, which in turn lets `["cmake"] + config.cmake_args`
    // keep its element type all the way to the `step` that consumes it.
    // Without it, every config read poisons the expression it appears in.
    TypeChecker(std::vector<std::string>&       errors,
                const std::vector<SchemaField>& fields,
                bool                            has_schema_block)
        : errors_(errors), has_schema_block_(has_schema_block)
    {
        for (const SchemaField& field : fields) {
            config_fields_[field.name] = schema_type(field);
            if (field.type == "enum")
                config_enums_[field.name] = field.enum_values;
        }
    }

    void command(const Command& command)
    {
        // Mirror of Evaluator::run(): only the declared parameters are in
        // scope, so a name the command did not ask for is reported here at
        // load time rather than surfacing as a run-time surprise.
        static const std::map<std::string, StaticType> kHostArguments = {
            {"project", StaticType::Record},
            {"config", StaticType::Record},
            {"extra", StaticType::ListString},
        };

        values_.clear();
        for (const std::string& parameter : command.parameters) {
            const auto host = kHostArguments.find(parameter);
            if (host == kHostArguments.end()) {
                error("unknown command parameter '" + parameter +
                          "'; expected one of project, config, extra",
                      command.token);
                values_[parameter] = StaticType::Unknown;
                continue;
            }
            values_[parameter] = host->second;
        }
        for (const Statement& statement : command.body.statements)
            statement_check(statement);
    }

private:
    void error(const std::string& message, const Token& token)
    {
        errors_.push_back(message + " at line " + std::to_string(token.line) + ":" +
                          std::to_string(token.column));
    }

    StaticType expression(const Expr& expr)
    {
        switch (expr.kind) {
            case Expr::Kind::String:
                return StaticType::String;
            case Expr::Kind::Integer:
                return StaticType::Integer;
            case Expr::Kind::Boolean:
                return StaticType::Boolean;
            case Expr::Kind::None:
                return StaticType::None;
            case Expr::Kind::Name:
                {
                    const auto found = values_.find(expr.token.text);
                    if (found == values_.end()) {
                        error("unknown name '" + expr.token.text + "'", expr.token);
                        return StaticType::Unknown;
                    }
                    return found->second;
                }
            case Expr::Kind::List:
                {
                    bool all_strings = true;
                    for (const Expr& child : expr.children)
                        all_strings = all_strings && expression(child) == StaticType::String;
                    return all_strings ? StaticType::ListString : StaticType::ListUnknown;
                }
            case Expr::Kind::Record:
                for (const Expr& child : expr.children)
                    expression(child);
                return StaticType::Record;
            case Expr::Kind::Member:
                {
                    const StaticType object = expression(expr.children.front());
                    if (object != StaticType::Record && object != StaticType::Unknown)
                        error("member access requires a record", expr.token);
                    const Expr& base = expr.children.front();
                    if (base.kind == Expr::Kind::Name && base.token.text == "project") {
                        if (expr.token.text == "root")
                            return StaticType::String;
                        if (expr.token.text == "matched_files")
                            return StaticType::ListString;
                        // The callable members are resolved in call(); getting
                        // here means one was used as a bare value.
                        if (is_project_method(expr.token.text))
                            error("project." + expr.token.text +
                                      " is a function; call it with arguments",
                                  expr.token);
                        else
                            error("unknown project member '" + expr.token.text + "'", expr.token);
                        return StaticType::Unknown;
                    }
                    if (base.kind == Expr::Kind::Name && base.token.text == "config") {
                        // Only a plugin that actually declares a schema gets
                        // its config keys checked. Without a schema block
                        // there is no declared surface to check against, so
                        // reads stay Unknown rather than being reported as
                        // unknown keys.
                        if (!has_schema_block_)
                            return StaticType::Unknown;
                        const auto field = config_fields_.find(expr.token.text);
                        if (field == config_fields_.end()) {
                            error("unknown config key '" + expr.token.text +
                                      "'; it is not declared in the schema block",
                                  expr.token);
                            return StaticType::Unknown;
                        }
                        return field->second;
                    }
                    return StaticType::Unknown;
                }
            case Expr::Kind::Index:
                {
                    const StaticType object = expression(expr.children[0]);
                    const StaticType index  = expression(expr.children[1]);
                    if (index != StaticType::Integer && index != StaticType::Unknown)
                        error("list index must be an integer", expr.children[1].token);
                    if (object == StaticType::ListString)
                        return StaticType::String;
                    if (object == StaticType::ListInteger)
                        return StaticType::Integer;
                    if (object != StaticType::ListUnknown && object != StaticType::Unknown)
                        error("index access requires a list", expr.token);
                    return StaticType::Unknown;
                }
            case Expr::Kind::Unary:
                {
                    const StaticType operand = expression(expr.children.front());
                    if (expr.token.kind == TokenKind::Bang) {
                        if (operand != StaticType::Boolean && operand != StaticType::Unknown)
                            error("unary '!' requires a boolean", expr.token);
                        return StaticType::Boolean;
                    }
                    if (operand != StaticType::Integer && operand != StaticType::Unknown)
                        error("unary '-' requires an integer", expr.token);
                    return StaticType::Integer;
                }
            case Expr::Kind::Binary:
                return binary(expr);
            case Expr::Kind::Conditional:
                {
                    const StaticType condition = expression(expr.children[0]);
                    if (condition != StaticType::Boolean && condition != StaticType::Unknown)
                        error("conditional condition must be a boolean", expr.children[0].token);
                    const StaticType chosen   = expression(expr.children[1]);
                    const StaticType fallback = expression(expr.children[2]);
                    return chosen == fallback ? chosen : StaticType::Unknown;
                }
            case Expr::Kind::Call:
                return call(expr);
            case Expr::Kind::Match:
                return match_check(expr);
        }
        return StaticType::Unknown;
    }

    // Check a match expression and give it a type.
    //
    // The arms' result type is the match's type when every arm agrees, and
    // Unknown otherwise — that is what makes `let gen = match ... { ... }`
    // followed by `step gen` check properly.
    //
    // When the subject is a `config` field declared as an enum, coverage is
    // checked here: design doc §5.9 promises "exhaustiveness checked when arms
    // cover enum", and this is the only place that knows both the arms and the
    // declared member list.
    StaticType match_check(const Expr& expr)
    {
        const Expr&      subject      = expr.children.front();
        const StaticType subject_type = expression(subject);

        std::vector<std::string> covered;
        StaticType               result = StaticType::Unknown;
        bool                     first  = true;
        bool                     agreed = true;

        for (std::size_t index = 1; index + 1 < expr.children.size(); index += 2) {
            const Expr& pattern = expr.children[index];
            if (pattern.kind == Expr::Kind::Name) {
                // A bare word pattern is an enum member; it only makes sense
                // against a string-typed subject.
                if (subject_type != StaticType::String && subject_type != StaticType::Unknown)
                    error("pattern '" + pattern.token.text +
                              "' matches a string, but the subject is " +
                              std::string(type_name(subject_type)),
                          pattern.token);
                covered.push_back(pattern.token.text);
            } else {
                const StaticType pattern_type = expression(pattern);
                if (pattern_type != subject_type && pattern_type != StaticType::Unknown &&
                    subject_type != StaticType::Unknown && pattern_type != StaticType::None)
                    error("pattern is " + std::string(type_name(pattern_type)) +
                              " but the subject is " + std::string(type_name(subject_type)),
                          pattern.token);
            }

            const StaticType arm = expression(expr.children[index + 1]);
            if (first) {
                result = arm;
                first  = false;
            } else if (arm != result) {
                agreed = false;
            }
        }

        check_enum_exhaustiveness(subject, covered, expr.token);
        return agreed ? result : StaticType::Unknown;
    }

    void check_enum_exhaustiveness(const Expr&                     subject,
                                   const std::vector<std::string>& covered,
                                   const Token&                    token)
    {
        // Only `config.<field>` subjects can be checked: that is the only
        // expression whose closed set of values is declared anywhere.
        if (subject.kind != Expr::Kind::Member ||
            subject.children.front().kind != Expr::Kind::Name ||
            subject.children.front().token.text != "config")
            return;
        const auto declared = config_enums_.find(subject.token.text);
        if (declared == config_enums_.end())
            return;

        std::vector<std::string> missing;
        for (const std::string& member : declared->second)
            if (std::find(covered.begin(), covered.end(), member) == covered.end())
                missing.push_back(member);
        if (missing.empty())
            return;

        std::string list;
        for (const std::string& member : missing)
            list += (list.empty() ? "" : ", ") + member;
        error("match on config." + subject.token.text + " does not cover " + list, token);
    }

    StaticType binary(const Expr& expr)
    {
        const StaticType left  = expression(expr.children[0]);
        const StaticType right = expression(expr.children[1]);
        const TokenKind  op    = expr.token.kind;
        if (op == TokenKind::AndAnd || op == TokenKind::OrOr) {
            require_boolean(left, expr.children[0].token);
            require_boolean(right, expr.children[1].token);
            return StaticType::Boolean;
        }
        if (op == TokenKind::EqualEqual || op == TokenKind::BangEqual)
            return StaticType::Boolean;

        const bool comparison = op == TokenKind::Less || op == TokenKind::LessEqual ||
                                op == TokenKind::Greater || op == TokenKind::GreaterEqual;
        const bool unknown = left == StaticType::Unknown || right == StaticType::Unknown;

        // `<` `<=` `>` `>=` are integer-only and always answer a boolean.
        if (comparison) {
            if (!unknown && (left != StaticType::Integer || right != StaticType::Integer))
                error("comparison requires integers, got " + std::string(type_name(left)) +
                          " and " + std::string(type_name(right)),
                      expr.token);
            return StaticType::Boolean;
        }

        // `+` is overloaded: string concat, list concat, integer addition
        // (design doc §5.8). Every other arithmetic operator is integers only.
        if (op == TokenKind::Plus) {
            if (left == right && (left == StaticType::String || left == StaticType::ListString ||
                                  left == StaticType::ListInteger || left == StaticType::Integer))
                return left;
            // List concatenation where one side's element type is not known
            // statically. This happens for an ordinary literal such as
            // `["-G", gen]` when `gen` came from a match whose arms disagree,
            // and refusing it would rule out design doc §5.3's generator
            // selection. The known side wins so the element type survives.
            const bool left_list  = is_list(left);
            const bool right_list = is_list(right);
            if (left_list && right_list) {
                if (left == StaticType::ListUnknown)
                    return right;
                if (right == StaticType::ListUnknown)
                    return left;
                error("cannot concatenate " + std::string(type_name(left)) + " and " +
                          std::string(type_name(right)),
                      expr.token);
                return StaticType::ListUnknown;
            }
            // With one side Unknown, the known side is the best available
            // answer.
            //
            // The bug this replaces: the old code fell through to a clause
            // that returned Integer whenever *either* operand was Unknown, so
            // `["cmake"] + config.cmake_args` typed as an integer and the
            // `step` consuming it was rejected with "step arguments must be
            // strings, got integer" — a false positive on the exact idiom the
            // design doc writes in §5.3.
            if (unknown)
                return left == StaticType::Unknown ? right : left;
            error("cannot add " + std::string(type_name(left)) + " and " +
                      std::string(type_name(right)),
                  expr.token);
            return StaticType::Unknown;
        }

        // `-` `*` `/`
        if (unknown)
            return StaticType::Integer;
        if (left == StaticType::Integer && right == StaticType::Integer)
            return StaticType::Integer;
        error("arithmetic requires integers, got " + std::string(type_name(left)) + " and " +
                  std::string(type_name(right)),
              expr.token);
        return StaticType::Unknown;
    }

    static bool is_list(StaticType type)
    {
        return type == StaticType::ListString || type == StaticType::ListInteger ||
               type == StaticType::ListUnknown;
    }

    // The host methods on `project` (design doc §3.4 / §5.8).
    static bool is_project_method(const std::string& name)
    {
        return project_methods().count(name) != 0;
    }

    void require_boolean(StaticType type, const Token& token)
    {
        if (type != StaticType::Boolean && type != StaticType::Unknown)
            error("condition must be a boolean, got " + std::string(type_name(type)), token);
    }

    // One signature table, shared by the checker and mirrored by the
    // interpreter's dispatch. `Unknown` as a return type means "the lattice
    // cannot express it" — `project.env` returns `str?`, which is a string or
    // none depending on the environment.
    struct Signature
    {
        std::vector<StaticType> parameters;
        StaticType              result;
    };

    static const std::map<std::string, Signature>& project_methods()
    {
        static const std::map<std::string, Signature> kMethods = {
            {"exists", {{StaticType::String}, StaticType::Boolean}},
            {"tool", {{StaticType::String}, StaticType::Boolean}},
            {"read", {{StaticType::String}, StaticType::String}},
            {"glob", {{StaticType::String}, StaticType::ListString}},
            {"env", {{StaticType::String}, StaticType::Unknown}},
        };
        return kMethods;
    }

    static const std::map<std::string, Signature>& stdlib_functions()
    {
        static const std::map<std::string, Signature> kFunctions = {
            {"len", {{StaticType::ListUnknown}, StaticType::Integer}},
            {"contains", {{StaticType::String, StaticType::String}, StaticType::Boolean}},
            {"trim", {{StaticType::String}, StaticType::String}},
            {"split", {{StaticType::String, StaticType::String}, StaticType::ListString}},
        };
        return kFunctions;
    }

    // Is `actual` acceptable where `expected` is declared? Unknown is the
    // wildcard in both directions, and any list satisfies a `list` parameter.
    static bool assignable(StaticType expected, StaticType actual)
    {
        if (expected == StaticType::Unknown || actual == StaticType::Unknown)
            return true;
        if (expected == StaticType::ListUnknown)
            return actual == StaticType::ListString || actual == StaticType::ListInteger ||
                   actual == StaticType::ListUnknown;
        return expected == actual;
    }

    StaticType check_signature(const Signature&               signature,
                               const std::string&             name,
                               const std::vector<StaticType>& arguments,
                               const Token&                   token)
    {
        if (arguments.size() != signature.parameters.size()) {
            error("'" + name + "' takes " + std::to_string(signature.parameters.size()) +
                      " argument" + (signature.parameters.size() == 1 ? "" : "s") + ", got " +
                      std::to_string(arguments.size()),
                  token);
            return signature.result;
        }
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (assignable(signature.parameters[index], arguments[index]))
                continue;
            error("argument " + std::to_string(index + 1) + " of '" + name + "' must be " +
                      std::string(type_name(signature.parameters[index])) + ", got " +
                      std::string(type_name(arguments[index])),
                  token);
        }
        return signature.result;
    }

    StaticType call(const Expr& expr)
    {
        const Expr& callee = expr.children.front();

        std::vector<StaticType> arguments;
        arguments.reserve(expr.children.size() - 1);
        for (std::size_t index = 1; index < expr.children.size(); ++index)
            arguments.push_back(expression(expr.children[index]));

        if (callee.kind == Expr::Kind::Name) {
            const auto found = stdlib_functions().find(callee.token.text);
            if (found == stdlib_functions().end()) {
                error("unknown function '" + callee.token.text + "'", callee.token);
                return StaticType::Unknown;
            }
            return check_signature(found->second, callee.token.text, arguments, callee.token);
        }
        if (callee.kind == Expr::Kind::Member && callee.children.front().kind == Expr::Kind::Name &&
            callee.children.front().token.text == "project") {
            const auto found = project_methods().find(callee.token.text);
            if (found == project_methods().end()) {
                error("unknown project method '" + callee.token.text + "'", callee.token);
                return StaticType::Unknown;
            }
            return check_signature(
                found->second, "project." + callee.token.text, arguments, callee.token);
        }
        error("only project.* methods and stdlib functions can be called", expr.token);
        return StaticType::Unknown;
    }

    // The record form of `step` (design doc §5.4) is checked field by field
    // rather than as a generic record, so a misspelled `cwd` is caught at
    // plugin-load time instead of being silently dropped at run time.
    void step_record_check(const Expr& record)
    {
        bool has_command = false;
        for (std::size_t index = 0; index < record.names.size(); ++index) {
            const std::string& field = record.names[index];
            const Expr&        value = record.children[index];
            const StaticType   type  = expression(value);
            if (field == "cmd") {
                has_command = true;
                if (type != StaticType::ListString && type != StaticType::Unknown &&
                    type != StaticType::ListUnknown)
                    error("step field 'cmd' must be list<string>, got " +
                              std::string(type_name(type)),
                          value.token);
            } else if (field == "cwd" || field == "label") {
                if (type != StaticType::String && type != StaticType::Unknown)
                    error("step field '" + field + "' must be a string, got " +
                              std::string(type_name(type)),
                          value.token);
            } else if (field == "env") {
                if (type != StaticType::Record && type != StaticType::Unknown)
                    error("step field 'env' must be a record, got " + std::string(type_name(type)),
                          value.token);
            } else {
                error("unknown step field '" + field + "'; expected cmd, cwd, env, or label",
                      value.token);
            }
        }
        if (!has_command)
            error("step record requires a 'cmd' field", record.token);
    }

    void step_check(const Statement& statement)
    {
        if (statement.expressions.empty()) {
            error("step requires a non-empty command", statement.token);
            return;
        }
        if (statement.expressions.size() == 1 &&
            statement.expressions.front().kind == Expr::Kind::Record) {
            step_record_check(statement.expressions.front());
            return;
        }
        for (const Expr& argument : statement.expressions) {
            if (argument.kind == Expr::Kind::Record) {
                error("a record step must be the only argument to `step`", argument.token);
                continue;
            }
            // Mirror of Evaluator::step_argument(): a lone unbound identifier
            // in step position is a bare program word, not an unknown name.
            if (argument.kind == Expr::Kind::Name && values_.count(argument.token.text) == 0)
                continue;
            const StaticType type = expression(argument);
            if (type != StaticType::String && type != StaticType::ListString &&
                type != StaticType::ListUnknown && type != StaticType::Unknown)
                error("step arguments must be strings, got " + std::string(type_name(type)),
                      argument.token);
        }
    }

    void statement_check(const Statement& statement)
    {
        switch (statement.kind) {
            case Statement::Kind::Assignment:
                // `x = ...` updates an existing binding; `let x = ...` creates
                // one. Requiring the binding to exist turns a misspelled
                // variable into a diagnostic instead of a silent second
                // variable that nothing ever reads.
                //
                // KPL is flat-scoped, so a `let` inside an `if` body counts as
                // a binding here even when that branch is not taken at run
                // time. The interpreter applies the same rule and would fail in
                // that case, which is a shape no readable plugin writes.
                if (values_.count(statement.name) == 0)
                    error("assignment to undeclared name '" + statement.name + "'; use `let " +
                              statement.name + " = ...` to declare it",
                          statement.token);
                values_[statement.name] = expression(statement.expressions.front());
                return;
            case Statement::Kind::Let:
                values_[statement.name] = expression(statement.expressions.front());
                return;
            case Statement::Kind::Step:
                step_check(statement);
                return;
            case Statement::Kind::If:
                require_boolean(expression(statement.expressions.front()), statement.token);
                for (const Statement& child : statement.body)
                    statement_check(child);
                for (const Statement& child : statement.otherwise)
                    statement_check(child);
                return;
            case Statement::Kind::For:
                {
                    const StaticType iterable = expression(statement.expressions.front());
                    // `for` iterates lists only (design doc §5.9: no numeric ranges
                    // in v1), so the loop variable's type follows the element type.
                    if (iterable != StaticType::ListString && iterable != StaticType::ListInteger &&
                        iterable != StaticType::ListUnknown && iterable != StaticType::Unknown)
                        error("for loop requires a list, got " + std::string(type_name(iterable)),
                              statement.expressions.front().token);
                    // Same scoping rule as Evaluator::for_run(): the loop
                    // variable exists only inside the loop, so a use after it
                    // is reported here as an unknown name rather than becoming
                    // a run-time surprise.
                    const auto                      outer = values_.find(statement.name);
                    const std::optional<StaticType> shadowed =
                        outer == values_.end() ? std::nullopt
                                               : std::optional<StaticType>(outer->second);

                    values_[statement.name] =
                        iterable == StaticType::ListString    ? StaticType::String
                        : iterable == StaticType::ListInteger ? StaticType::Integer
                                                              : StaticType::Unknown;
                    for (const Statement& child : statement.body)
                        statement_check(child);

                    if (shadowed)
                        values_[statement.name] = *shadowed;
                    else
                        values_.erase(statement.name);
                    return;
                }
            case Statement::Kind::Match:
            case Statement::Kind::Concurrent:
                require_boolean(expression(statement.expressions.front()), statement.token);
                return;
            case Statement::Kind::ReportFreedSpace:
            case Statement::Kind::Expression:
                for (const Expr& expr : statement.expressions)
                    expression(expr);
                return;
            case Statement::Kind::Directive:
                // Directives only appear in `detect` and `requires`, which the
                // command type checker never walks.
                return;
        }
    }

    std::vector<std::string>&                       errors_;
    bool                                            has_schema_block_ = false;
    std::map<std::string, StaticType>               config_fields_;
    std::map<std::string, std::vector<std::string>> config_enums_;
    std::map<std::string, StaticType>               values_;
};

} // namespace

std::vector<std::string> type_check(const Plugin& plugin)
{
    std::vector<std::string>       errors;
    const std::vector<SchemaField> fields = schema(plugin);
    for (const Command& command : plugin.commands)
        TypeChecker{errors, fields, plugin.schema.has_value()}.command(command);
    return errors;
}

namespace
{

class Evaluator
{
public:
    Evaluator(std::string                         source_name,
              const Project&                      project,
              const std::map<std::string, Value>& config,
              const std::vector<std::string>&     extra)
        : source_name_(std::move(source_name)), project_(&project)
    {
        // The three host values a command may ask for, by the names the design
        // doc uses in every example (§5.3). They are staged here and bound in
        // run(), because *which* of them are in scope depends on the command's
        // own parameter list.
        arguments_["config"]  = Value::record_value(config);
        arguments_["extra"]   = strings_to_value(extra);
        arguments_["project"] = Value::record_value({
            {"root", Value::string_value(project.root)},
            {"matched_files", strings_to_value(project.matched_files)},
        });
    }

    CommandSpec run(const Command& command)
    {
        // Bind exactly the parameters the command declares — no more.
        //
        // The environment used to be pre-loaded with all three host values
        // unconditionally, so `command clean(project, config)` could still
        // read `extra` and get the passthrough arguments it never asked for.
        // That is a silent scoping hole: a plugin would work locally and then
        // behave differently the moment someone corrected its signature.
        for (const std::string& parameter : command.parameters) {
            const auto argument = arguments_.find(parameter);
            if (argument == arguments_.end())
                fail("unknown command parameter '" + parameter +
                         "'; expected one of project, config, extra",
                     command.token);
            environment_[parameter] = argument->second;
        }
        for (const Statement& statement : command.body.statements)
            statement_run(statement);
        return spec_;
    }

private:
    static Value strings_to_value(const std::vector<std::string>& values)
    {
        std::vector<Value> result;
        result.reserve(values.size());
        for (const std::string& value : values)
            result.push_back(Value::string_value(value));
        return Value::list_value(std::move(result));
    }

    [[noreturn]] void fail(const std::string& message, const Token& token) const
    {
        throw diag::Error{
            diag::error(message, diag::Location{source_name_, token.line, token.column})};
    }

    // Unwrap a value that must be a boolean.
    //
    // KPL has no truthiness coercion (design doc §5.6 makes `bool` its own
    // type), so anything else here is a plugin bug. The predecessor of this
    // helper, `truthy()`, quietly answered "false" for every non-boolean,
    // which turned `if config.build_dir { ... }` — a real mistake — into a
    // silently skipped branch instead of a diagnostic.
    bool boolean(const Value& value, const Token& token) const
    {
        if (value.kind != Value::Kind::Boolean)
            fail("expected a boolean, got " + std::string(kind_name(value.kind)), token);
        return value.boolean;
    }

    static const char* kind_name(Value::Kind kind)
    {
        switch (kind) {
            case Value::Kind::None:
                return "none";
            case Value::Kind::String:
                return "a string";
            case Value::Kind::Integer:
                return "an integer";
            case Value::Kind::Boolean:
                return "a boolean";
            case Value::Kind::List:
                return "a list";
            case Value::Kind::Record:
                return "a record";
        }
        return "a value";
    }

    // Deep structural equality for `==` / `!=`.
    //
    // The first implementation compared every scalar field of Value side by
    // side (`left.string == right.string && left.integer == ...`). For two
    // lists that test looks at the *unused* scalar fields — both empty — and
    // never at the elements, so `["a"] == ["b"]` evaluated to true. Comparing
    // per kind is the only way to get this right.
    static bool equal(const Value& left, const Value& right)
    {
        if (left.kind != right.kind)
            return false;
        switch (left.kind) {
            case Value::Kind::None:
                return true;
            case Value::Kind::String:
                return left.string == right.string;
            case Value::Kind::Integer:
                return left.integer == right.integer;
            case Value::Kind::Boolean:
                return left.boolean == right.boolean;
            case Value::Kind::List:
                if (left.list.size() != right.list.size())
                    return false;
                for (std::size_t index = 0; index < left.list.size(); ++index)
                    if (!equal(left.list[index], right.list[index]))
                        return false;
                return true;
            case Value::Kind::Record:
                {
                    if (left.record.size() != right.record.size())
                        return false;
                    // Both maps are std::map, so a lock-step walk visits the same
                    // key order on each side.
                    auto right_field = right.record.begin();
                    for (const auto& [name, value] : left.record) {
                        if (right_field->first != name || !equal(value, right_field->second))
                            return false;
                        ++right_field;
                    }
                    return true;
                }
        }
        return false;
    }

    // One word of an argv array. Steps are argv arrays of strings, never a
    // shell string (design doc §5.4), so nothing is coerced here: an integer
    // in a step is a plugin bug, not something to stringify silently.
    const std::string& step_word(const Value& value, const Token& token) const
    {
        if (value.kind != Value::Kind::String)
            fail("step arguments must be strings, got " + std::string(kind_name(value.kind)),
                 token);
        return value.string;
    }

    Value expression(const Expr& expr)
    {
        switch (expr.kind) {
            case Expr::Kind::String:
                return Value::string_value(expr.token.text);
            case Expr::Kind::Integer:
                return Value::integer_value(expr.token.integer);
            case Expr::Kind::Boolean:
                return Value::boolean_value(expr.token.text == "true");
            case Expr::Kind::None:
                return Value::none();
            case Expr::Kind::Name:
                {
                    const auto found = environment_.find(expr.token.text);
                    if (found == environment_.end())
                        fail("unknown name '" + expr.token.text + "'", expr.token);
                    return found->second;
                }
            case Expr::Kind::List:
                {
                    std::vector<Value> values;
                    for (const Expr& child : expr.children)
                        values.push_back(expression(child));
                    return Value::list_value(std::move(values));
                }
            case Expr::Kind::Record:
                {
                    std::map<std::string, Value> values;
                    for (std::size_t i = 0; i < expr.names.size(); ++i)
                        values.emplace(expr.names[i], expression(expr.children[i]));
                    return Value::record_value(std::move(values));
                }
            case Expr::Kind::Member:
                {
                    const Value object = expression(expr.children.front());
                    if (object.kind != Value::Kind::Record)
                        fail("member access requires a record", expr.token);
                    const auto found = object.record.find(expr.token.text);
                    if (found == object.record.end())
                        fail("unknown member '" + expr.token.text + "'", expr.token);
                    return found->second;
                }
            case Expr::Kind::Index:
                {
                    const Value object = expression(expr.children[0]);
                    const Value index  = expression(expr.children[1]);
                    if (object.kind != Value::Kind::List || index.kind != Value::Kind::Integer ||
                        index.integer < 0 ||
                        static_cast<std::size_t>(index.integer) >= object.list.size())
                        fail("list index is out of range", expr.token);
                    return object.list[static_cast<std::size_t>(index.integer)];
                }
            case Expr::Kind::Unary:
                {
                    const Value value = expression(expr.children.front());
                    if (expr.token.kind == TokenKind::Bang)
                        return Value::boolean_value(!boolean(value, expr.token));
                    if (value.kind != Value::Kind::Integer)
                        fail("unary '-' requires an integer", expr.token);
                    return Value::integer_value(-value.integer);
                }
            case Expr::Kind::Binary:
                return binary(expr);
            case Expr::Kind::Conditional:
                return boolean(expression(expr.children[0]), expr.children[0].token)
                           ? expression(expr.children[1])
                           : expression(expr.children[2]);
            case Expr::Kind::Call:
                return call(expr);
            case Expr::Kind::Match:
                return match_run(expr);
        }
        fail("unsupported expression", expr.token);
    }

    // `match <subject> { pattern => result, ... }` (design doc §5.9).
    //
    // Arms are tried in source order and the first match wins. There is no
    // catch-all pattern in v1: an unmatched value is an error naming the value
    // and the file, which is the honest outcome when a plugin adds an enum
    // member and forgets an arm.
    Value match_run(const Expr& expr)
    {
        const Value subject = expression(expr.children.front());
        for (std::size_t index = 1; index + 1 < expr.children.size(); index += 2) {
            if (pattern_matches(expr.children[index], subject))
                return expression(expr.children[index + 1]);
        }
        fail("no match arm covers " + describe(subject), expr.token);
    }

    // A Name pattern is a bare enum member compared against the subject's
    // text — `ninja => ...` matches the string "ninja". Everything else is a
    // literal compared structurally.
    bool pattern_matches(const Expr& pattern, const Value& subject)
    {
        if (pattern.kind == Expr::Kind::Name)
            return subject.kind == Value::Kind::String && subject.string == pattern.token.text;
        return equal(expression(pattern), subject);
    }

    // A short rendering of a value for diagnostics; not a serialization format.
    static std::string describe(const Value& value)
    {
        switch (value.kind) {
            case Value::Kind::String:
                return "\"" + value.string + "\"";
            case Value::Kind::Integer:
                return std::to_string(value.integer);
            case Value::Kind::Boolean:
                return value.boolean ? "true" : "false";
            default:
                return kind_name(value.kind);
        }
    }

    Value binary(const Expr& expr)
    {
        // `&&` and `||` are handled first and completely, because they are the
        // only operators that must NOT evaluate their right operand up front.
        // Short-circuiting is what makes
        //     project.exists("package.json") && contains(project.read(...), "x")
        // safe to write: the read never happens when the file is missing.
        //
        // The bug this replaces: the old code returned early only on the
        // short-circuit path and then fell through to the arithmetic/compare
        // ladder, which knows nothing about AndAnd/OrOr. So `true && true`
        // reached the bottom and failed with "incompatible operands" — every
        // non-short-circuiting logical expression in every plugin was broken.
        if (expr.token.kind == TokenKind::AndAnd || expr.token.kind == TokenKind::OrOr) {
            const bool left = boolean(expression(expr.children[0]), expr.children[0].token);
            if (expr.token.kind == TokenKind::AndAnd && !left)
                return Value::boolean_value(false);
            if (expr.token.kind == TokenKind::OrOr && left)
                return Value::boolean_value(true);
            return Value::boolean_value(
                boolean(expression(expr.children[1]), expr.children[1].token));
        }

        const Value left  = expression(expr.children[0]);
        const Value right = expression(expr.children[1]);
        if (expr.token.kind == TokenKind::Plus) {
            if (left.kind == Value::Kind::String && right.kind == Value::Kind::String)
                return Value::string_value(left.string + right.string);
            if (left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
                std::vector<Value> values = left.list;
                values.insert(values.end(), right.list.begin(), right.list.end());
                return Value::list_value(std::move(values));
            }
        }
        if (left.kind == Value::Kind::Integer && right.kind == Value::Kind::Integer) {
            switch (expr.token.kind) {
                case TokenKind::Plus:
                    return Value::integer_value(left.integer + right.integer);
                case TokenKind::Minus:
                    return Value::integer_value(left.integer - right.integer);
                case TokenKind::Star:
                    return Value::integer_value(left.integer * right.integer);
                case TokenKind::Slash:
                    if (right.integer == 0)
                        fail("division by zero", expr.token);
                    return Value::integer_value(left.integer / right.integer);
                case TokenKind::Less:
                    return Value::boolean_value(left.integer < right.integer);
                case TokenKind::LessEqual:
                    return Value::boolean_value(left.integer <= right.integer);
                case TokenKind::Greater:
                    return Value::boolean_value(left.integer > right.integer);
                case TokenKind::GreaterEqual:
                    return Value::boolean_value(left.integer >= right.integer);
                default:
                    break;
            }
        }
        if (expr.token.kind == TokenKind::EqualEqual || expr.token.kind == TokenKind::BangEqual) {
            const bool same = equal(left, right);
            return Value::boolean_value(expr.token.kind == TokenKind::EqualEqual ? same : !same);
        }
        fail("incompatible operands", expr.token);
    }

    // --- calls -------------------------------------------------------------
    //
    // KPL has no user-defined functions (design doc §5.8). The only callables
    // are the `project.*` host methods and a fixed stdlib, which is what keeps
    // the sandbox auditable: this function is the complete list of things a
    // plugin can make the host do.

    void
    check_arity(const std::vector<Value>& arguments, std::size_t expected, const Token& name) const
    {
        if (arguments.size() != expected)
            fail("'" + name.text + "' takes " + std::to_string(expected) + " argument" +
                     (expected == 1 ? "" : "s") + ", got " + std::to_string(arguments.size()),
                 name);
    }

    const std::string&
    string_argument(const std::vector<Value>& arguments, std::size_t index, const Token& name) const
    {
        if (arguments[index].kind != Value::Kind::String)
            fail("argument " + std::to_string(index + 1) + " of '" + name.text +
                     "' must be a string, got " + kind_name(arguments[index].kind),
                 name);
        return arguments[index].string;
    }

    Value call(const Expr& expr)
    {
        const Expr& callee = expr.children.front();

        // Arguments are ordinary expressions, evaluated before dispatch.
        //
        // The bug this replaces: the old implementation read the argument
        // straight off the AST and required `expr.children[1].kind == String`,
        // so only a *literal* could be passed. The design doc's own workspace
        // example — `project.exists(ws + "/package.json")` (§5.9) — was
        // rejected as "project host calls require one string argument".
        std::vector<Value> arguments;
        arguments.reserve(expr.children.size() - 1);
        for (std::size_t index = 1; index < expr.children.size(); ++index)
            arguments.push_back(expression(expr.children[index]));

        if (callee.kind == Expr::Kind::Name)
            return stdlib_call(callee.token, arguments);
        if (callee.kind == Expr::Kind::Member && callee.children.front().kind == Expr::Kind::Name &&
            callee.children.front().token.text == "project")
            return project_call(callee.token, arguments);
        fail("only project.* methods and stdlib functions can be called", expr.token);
    }

    // The `project` host methods (design doc §3.4). Each one is a capability:
    // if the host did not supply the callback, the query answers safely rather
    // than reaching for the disk behind the sandbox's back.
    Value project_call(const Token& method, const std::vector<Value>& arguments)
    {
        if (method.text == "exists") {
            check_arity(arguments, 1, method);
            const std::string& path = string_argument(arguments, 0, method);
            return Value::boolean_value(project_->exists && project_->exists(path));
        }
        if (method.text == "tool") {
            check_arity(arguments, 1, method);
            const std::string& name = string_argument(arguments, 0, method);
            return Value::boolean_value(project_->tool && project_->tool(name));
        }
        if (method.text == "read") {
            check_arity(arguments, 1, method);
            const std::string& path = string_argument(arguments, 0, method);
            // No safe default: "" is indistinguishable from an empty file, so
            // a missing capability has to be reported rather than guessed.
            if (!project_->read)
                fail("project.read is not available here", method);
            return Value::string_value(project_->read(path));
        }
        if (method.text == "glob") {
            check_arity(arguments, 1, method);
            const std::string& pattern = string_argument(arguments, 0, method);
            if (!project_->glob)
                return Value::list_value({});
            return strings_to_value(project_->glob(pattern));
        }
        if (method.text == "env") {
            check_arity(arguments, 1, method);
            const std::string& name = string_argument(arguments, 0, method);
            if (!project_->env)
                return Value::none();
            // `str?`: an unset (or deny-listed) variable reads as `none`, so
            // plugins branch on it with `!= none` rather than on "".
            const std::optional<std::string> value = project_->env(name);
            return value ? Value::string_value(*value) : Value::none();
        }
        fail("unknown project method '" + method.text + "'", method);
    }

    // The free-function stdlib (design doc §5.8). Four functions, no more:
    // every addition here widens what a third-party plugin can do.
    Value stdlib_call(const Token& name, const std::vector<Value>& arguments)
    {
        if (name.text == "len") {
            check_arity(arguments, 1, name);
            if (arguments[0].kind != Value::Kind::List)
                fail("'len' requires a list, got " + std::string(kind_name(arguments[0].kind)),
                     name);
            return Value::integer_value(static_cast<std::int64_t>(arguments[0].list.size()));
        }
        if (name.text == "contains") {
            check_arity(arguments, 2, name);
            const std::string& haystack = string_argument(arguments, 0, name);
            const std::string& needle   = string_argument(arguments, 1, name);
            return Value::boolean_value(haystack.find(needle) != std::string::npos);
        }
        if (name.text == "trim") {
            check_arity(arguments, 1, name);
            const std::string& text  = string_argument(arguments, 0, name);
            const auto         space = [](unsigned char c) { return std::isspace(c) != 0; };
            std::size_t        begin = 0;
            std::size_t        end   = text.size();
            while (begin < end && space(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && space(static_cast<unsigned char>(text[end - 1])))
                --end;
            return Value::string_value(text.substr(begin, end - begin));
        }
        if (name.text == "split") {
            check_arity(arguments, 2, name);
            const std::string& text      = string_argument(arguments, 0, name);
            const std::string& separator = string_argument(arguments, 1, name);
            // An empty separator has no sensible answer (every position is a
            // match), and silently returning the input would hide the mistake.
            if (separator.empty())
                fail("'split' requires a non-empty separator", name);
            std::vector<Value> parts;
            std::size_t        begin = 0;
            for (;;) {
                const std::size_t hit = text.find(separator, begin);
                if (hit == std::string::npos) {
                    parts.push_back(Value::string_value(text.substr(begin)));
                    break;
                }
                parts.push_back(Value::string_value(text.substr(begin, hit - begin)));
                begin = hit + separator.size();
            }
            return Value::list_value(std::move(parts));
        }
        fail("unknown function '" + name.text + "'", name);
    }

    // `for x in <list> { ... }` — design doc §5.9. Lists only: there are no
    // numeric ranges in v1, and every list a plugin can reach is host-produced
    // and already capped (§7), so the loop cannot be driven unboundedly.
    //
    // The loop variable is scoped to the loop: whatever `x` meant outside is
    // restored afterwards. KPL is otherwise flat-scoped — an assignment inside
    // an `if` body deliberately updates the outer variable, which is how
    // §5.3's `cmake = cmake + [...]` works — but a loop variable leaking its
    // final element into the enclosing block is a bug source with no
    // corresponding use. The type checker applies the same rule.
    void for_run(const Statement& statement)
    {
        const Expr& iterable_expr = statement.expressions.front();
        const Value iterable      = expression(iterable_expr);
        if (iterable.kind != Value::Kind::List)
            fail("for loop requires a list, got " + std::string(kind_name(iterable.kind)),
                 iterable_expr.token);

        const auto                 outer = environment_.find(statement.name);
        const std::optional<Value> shadowed =
            outer == environment_.end() ? std::nullopt : std::optional<Value>(outer->second);

        for (const Value& item : iterable.list) {
            environment_[statement.name] = item;
            for (const Statement& child : statement.body)
                statement_run(child);
        }

        if (shadowed)
            environment_[statement.name] = *shadowed;
        else
            environment_.erase(statement.name);
    }

    // `step` has two forms (design doc §5.4):
    //
    //   step mkdir "-p" dir                      variadic argv
    //   step { cmd: [...], cwd: "x", label: "y" } record, with per-step options
    //
    // The record form is the only way to reach a step's cwd, env, and label,
    // which the executor needs for `kap dev`'s concurrent prefixed output
    // (§5.11). It was unreachable from KPL before: a record argument fell
    // through to the argv path and died with "step arguments must be strings".
    void step_run(const Statement& statement)
    {
        if (statement.expressions.size() == 1) {
            const Expr& only  = statement.expressions.front();
            const Value value = step_argument(only);
            if (value.kind == Value::Kind::Record) {
                spec_.steps.push_back(step_from_record(value, only.token));
                return;
            }
            spec_.steps.push_back(step_from_words({value}, {only.token}, statement.token));
            return;
        }

        std::vector<Value> values;
        std::vector<Token> tokens;
        values.reserve(statement.expressions.size());
        tokens.reserve(statement.expressions.size());
        for (const Expr& argument : statement.expressions) {
            values.push_back(step_argument(argument));
            tokens.push_back(argument.token);
        }
        spec_.steps.push_back(step_from_words(values, tokens, statement.token));
    }

    // Evaluate one `step` argument, applying the bare-word rule.
    //
    // Design doc §5.3 writes `step mkdir "-p" dir` and `step rm "-rf"
    // config.build_dir`: `mkdir` and `rm` are program names, `dir` is a
    // variable. Both are bare identifiers, so the only rule that makes both
    // examples work is "resolve it as a variable if one is bound, otherwise
    // take it as the literal word".
    //
    // The rule is deliberately narrow — it applies ONLY to an identifier
    // standing alone as a step argument, never inside a larger expression, so
    // `step [name]` or `step name + ".txt"` still demand a real binding. The
    // tradeoff it buys (a mistyped variable becomes a bare word instead of an
    // error) is confined to the one position where the design doc requires it.
    Value step_argument(const Expr& argument)
    {
        if (argument.kind == Expr::Kind::Name && environment_.count(argument.token.text) == 0)
            return Value::string_value(argument.token.text);
        return expression(argument);
    }

    // Flatten argv arguments: a string contributes one word, a list splices in
    // all of its words. That is what makes `step ["cargo", "build"] + extra`
    // and `step "ninja" dir` produce the same shape of argv.
    Step step_from_words(const std::vector<Value>& values,
                         const std::vector<Token>& tokens,
                         const Token&              statement_token) const
    {
        Step step;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const Value& value = values[index];
            if (value.kind == Value::Kind::List) {
                for (const Value& item : value.list)
                    step.command.push_back(step_word(item, tokens[index]));
            } else {
                step.command.push_back(step_word(value, tokens[index]));
            }
        }
        if (step.command.empty())
            fail("step requires a non-empty command", statement_token);
        return step;
    }

    Step step_from_record(const Value& record, const Token& token) const
    {
        Step step;
        for (const auto& [field, value] : record.record) {
            if (field == "cmd") {
                if (value.kind != Value::Kind::List)
                    fail("step field 'cmd' must be a list of strings", token);
                for (const Value& item : value.list)
                    step.command.push_back(step_word(item, token));
            } else if (field == "cwd") {
                step.cwd = step_word(value, token);
            } else if (field == "label") {
                step.label = step_word(value, token);
            } else if (field == "env") {
                if (value.kind != Value::Kind::Record)
                    fail("step field 'env' must be a record of strings", token);
                for (const auto& [name, entry] : value.record)
                    step.environment[name] = step_word(entry, token);
            } else {
                // An unknown field is almost always a typo for one of these
                // four, and silently ignoring it would drop the very option
                // the author was trying to set.
                fail("unknown step field '" + field + "'; expected cmd, cwd, env, or label", token);
            }
        }
        if (step.command.empty())
            fail("step record requires a non-empty 'cmd' list", token);
        return step;
    }

    void statement_run(const Statement& statement)
    {
        switch (statement.kind) {
            case Statement::Kind::Assignment:
                // Mirror of the type checker: assignment updates, `let`
                // declares.
                if (environment_.count(statement.name) == 0)
                    fail("assignment to undeclared name '" + statement.name + "'; use `let " +
                             statement.name + " = ...` to declare it",
                         statement.token);
                environment_[statement.name] = expression(statement.expressions.front());
                return;
            case Statement::Kind::Let:
                environment_[statement.name] = expression(statement.expressions.front());
                return;
            case Statement::Kind::Step:
                step_run(statement);
                return;
            case Statement::Kind::If:
                for (const Statement& child : boolean(expression(statement.expressions.front()),
                                                      statement.expressions.front().token)
                                                  ? statement.body
                                                  : statement.otherwise)
                    statement_run(child);
                return;
            case Statement::Kind::Concurrent:
                spec_.concurrent = boolean(expression(statement.expressions.front()),
                                           statement.expressions.front().token);
                return;
            case Statement::Kind::ReportFreedSpace:
                spec_.report_freed_space = true;
                return;
            case Statement::Kind::For:
                for_run(statement);
                return;
            case Statement::Kind::Directive:
                // Unreachable: directives live in `detect`/`requires`, which
                // are never evaluated as a command body.
                fail("a directive cannot appear inside a command", statement.token);
            case Statement::Kind::Match:
            case Statement::Kind::Expression:
                // Evaluated for its effects on the host (a `match` in
                // statement position is legal per the §5.5 grammar); the
                // resulting value is discarded.
                for (const Expr& expr : statement.expressions)
                    expression(expr);
                return;
        }
    }

    std::string                  source_name_;
    const Project*               project_ = nullptr;
    std::map<std::string, Value> arguments_;
    std::map<std::string, Value> environment_;
    CommandSpec                  spec_;
};

} // namespace

CommandSpec evaluate(const Plugin&                       plugin,
                     std::string_view                    command_name,
                     const Project&                      project,
                     const std::map<std::string, Value>& config,
                     const std::vector<std::string>&     extra)
{
    for (const Command& command : plugin.commands) {
        if (command.name == command_name)
            return Evaluator{plugin.source_name, project, config, extra}.run(command);
    }
    throw diag::Error{diag::error("unknown command '" + std::string(command_name) + "'")};
}

namespace
{

// --- the production host object (design doc §3.4 + §7) -----------------------

// Resolve a plugin-supplied path against the project root and refuse anything
// that leaves it.
//
// `weakly_canonical` inside fs::is_within resolves both `..` components and
// symlinks, so neither "../../etc/passwd" nor a symlink planted in the project
// can be used to read outside the tree. The check is here, once, rather than
// in each callback, because "every path a plugin names is validated" is only a
// guarantee if there is exactly one door.
std::optional<std::filesystem::path> resolve_in_root(const std::filesystem::path& root,
                                                     std::string_view             relative)
{
    if (relative.empty())
        return std::nullopt;
    const std::filesystem::path candidate = root / std::filesystem::path(relative);
    if (!fs::is_within(root, candidate))
        return std::nullopt;
    return candidate;
}

// Is `name` an environment variable a plugin may read?
//
// A deny-list, not an allow-list, because plugins legitimately need arbitrary
// build-related variables (CC, CFLAGS, NODE_ENV, ...) and we cannot enumerate
// them. The patterns cover the shapes secrets actually take in CI
// environments; design doc §7 names the first three and leaves the list open.
bool environment_is_readable(std::string_view name)
{
    static constexpr std::string_view kDeniedSuffixes[] = {
        "_TOKEN", "_KEY", "_SECRET", "_PASSWORD", "_PASSWD", "_CREDENTIALS", "_SESSION"};
    static constexpr std::string_view kDeniedPrefixes[] = {"AWS_", "GITHUB_TOKEN", "NPM_TOKEN"};

    std::string upper(name);
    for (char& character : upper)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));

    for (const std::string_view suffix : kDeniedSuffixes)
        if (upper.size() >= suffix.size() &&
            upper.compare(upper.size() - suffix.size(), suffix.size(), suffix) == 0)
            return false;
    for (const std::string_view prefix : kDeniedPrefixes)
        if (upper.rfind(prefix, 0) == 0)
            return false;
    return true;
}

// Is `name` an executable on PATH? Answers without spawning anything — design
// doc §5.8 specifies "checks PATH without exec".
bool tool_on_path(std::string_view name)
{
    // A name with a slash is a path, not a PATH lookup; refuse it rather than
    // silently probing an absolute location a plugin should not be naming.
    if (name.empty() || name.find('/') != std::string_view::npos)
        return false;
    const char* path = std::getenv("PATH");
    if (path == nullptr)
        return false;

    const std::string_view search(path);
    std::size_t            begin = 0;
    while (begin <= search.size()) {
        const std::size_t      end     = search.find(':', begin);
        const std::string_view segment = search.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        // POSIX: an empty PATH element means the current directory.
        const std::filesystem::path directory =
            segment.empty() ? "." : std::filesystem::path(segment);
        const std::filesystem::path candidate = directory / std::filesystem::path(name);
        if (::access(candidate.c_str(), X_OK) == 0 && fs::is_file(candidate))
            return true;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return false;
}

} // namespace

Project host_project(std::string root, std::vector<std::string> matched_files)
{
    Project project;
    project.root          = root;
    project.matched_files = std::move(matched_files);

    const std::filesystem::path base(root);

    project.exists = [base](std::string_view path) {
        const auto resolved = resolve_in_root(base, path);
        return resolved.has_value() && fs::exists(*resolved);
    };

    project.read = [base](std::string_view path) -> std::string {
        const auto resolved = resolve_in_root(base, path);
        if (!resolved)
            throw diag::Error{
                diag::error("project.read('" + std::string(path) + "') escapes the project root")};
        // fs::read_text applies the 1 MiB cap from design doc §7.
        return fs::read_text(*resolved);
    };

    project.glob = [base](std::string_view pattern) -> std::vector<std::string> {
        // Patterns are "<directory>/<name-pattern>"; only the final component
        // may contain wildcards, which is all §5.11's "packages/*" needs and
        // keeps the walk bounded to a single directory.
        const std::size_t      slash = pattern.rfind('/');
        const std::string_view relative =
            slash == std::string_view::npos ? std::string_view{} : pattern.substr(0, slash);
        const std::string_view leaf =
            slash == std::string_view::npos ? pattern : pattern.substr(slash + 1);

        std::filesystem::path directory = base;
        if (!relative.empty()) {
            const auto resolved = resolve_in_root(base, relative);
            if (!resolved)
                return {};
            directory = *resolved;
        }

        std::vector<std::string> names = fs::glob(directory, leaf);
        if (relative.empty())
            return names;
        // Re-attach the directory so results stay usable as project-relative
        // paths, which is what §5.11 feeds straight back into project.exists.
        for (std::string& name : names)
            name = std::string(relative) + "/" + name;
        return names;
    };

    project.tool = [](std::string_view name) { return tool_on_path(name); };

    project.env = [](std::string_view name) -> std::optional<std::string> {
        if (!environment_is_readable(name))
            return std::nullopt;
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr)
            return std::nullopt;
        return std::string(value);
    };

    return project;
}

json::Value to_json(const CommandSpec& spec)
{
    std::vector<json::Value> steps;
    steps.reserve(spec.steps.size());
    for (const Step& step : spec.steps) {
        std::vector<json::Value> command;
        command.reserve(step.command.size());
        for (const std::string& word : step.command)
            command.push_back(json::make_string(word));

        std::map<std::string, json::Value> environment;
        for (const auto& [name, value] : step.environment)
            environment.emplace(name, json::make_string(value));

        steps.push_back(json::make_object({
            {"cmd", json::make_array(std::move(command))},
            {"cwd", step.cwd ? json::make_string(*step.cwd) : json::make_null()},
            {"env", json::make_object(std::move(environment))},
            {"label", step.label ? json::make_string(*step.label) : json::make_null()},
        }));
    }
    return json::make_object({
        {"steps", json::make_array(std::move(steps))},
        {"concurrent", json::make_boolean(spec.concurrent)},
        {"report_freed_space", json::make_boolean(spec.report_freed_space)},
    });
}

namespace
{

[[noreturn]] void json_shape_error(const std::string& message, const std::string& source_name)
{
    throw diag::Error{diag::error(message, diag::Location{source_name})};
}

// Read an optional boolean field, defaulting when absent so a golden file only
// has to spell out what it cares about.
bool optional_boolean(const json::Value& object,
                      const char*        key,
                      bool               fallback,
                      const std::string& source_name)
{
    const json::Value* field = object.find(key);
    if (field == nullptr || field->kind == json::Value::Kind::Null)
        return fallback;
    if (field->kind != json::Value::Kind::Boolean)
        json_shape_error(std::string("'") + key + "' must be a boolean", source_name);
    return field->boolean;
}

} // namespace

CommandSpec spec_from_json(const json::Value& value, const std::string& source_name)
{
    if (value.kind != json::Value::Kind::Object)
        json_shape_error("a CommandSpec must be a JSON object", source_name);

    CommandSpec spec;
    spec.concurrent         = optional_boolean(value, "concurrent", false, source_name);
    spec.report_freed_space = optional_boolean(value, "report_freed_space", false, source_name);

    const json::Value* steps = value.find("steps");
    if (steps == nullptr)
        json_shape_error("a CommandSpec requires a 'steps' array", source_name);
    if (steps->kind != json::Value::Kind::Array)
        json_shape_error("'steps' must be an array", source_name);

    for (const json::Value& entry : steps->array) {
        if (entry.kind != json::Value::Kind::Object)
            json_shape_error("every step must be an object", source_name);

        Step               step;
        const json::Value* command = entry.find("cmd");
        if (command == nullptr || command->kind != json::Value::Kind::Array)
            json_shape_error("every step requires a 'cmd' array", source_name);
        for (const json::Value& word : command->array) {
            if (word.kind != json::Value::Kind::String)
                json_shape_error("'cmd' must contain only strings", source_name);
            step.command.push_back(word.string);
        }
        if (step.command.empty())
            json_shape_error("'cmd' must not be empty", source_name);

        for (const char* key : {"cwd", "label"}) {
            const json::Value* field = entry.find(key);
            if (field == nullptr || field->kind == json::Value::Kind::Null)
                continue;
            if (field->kind != json::Value::Kind::String)
                json_shape_error(std::string("'") + key + "' must be a string or null",
                                 source_name);
            (std::string(key) == "cwd" ? step.cwd : step.label) = field->string;
        }

        const json::Value* environment = entry.find("env");
        if (environment != nullptr && environment->kind != json::Value::Kind::Null) {
            if (environment->kind != json::Value::Kind::Object)
                json_shape_error("'env' must be an object", source_name);
            for (const auto& [name, entry_value] : environment->object) {
                if (entry_value.kind != json::Value::Kind::String)
                    json_shape_error("'env' values must be strings", source_name);
                step.environment[name] = entry_value.string;
            }
        }

        spec.steps.push_back(std::move(step));
    }
    return spec;
}

std::vector<std::string> validate(const Plugin& plugin, int supported_api_version)
{
    std::vector<std::string> errors;
    if (!plugin.manifest) {
        errors.push_back("missing manifest block");
        return errors;
    }

    bool has_name        = false;
    bool has_version     = false;
    bool has_api_version = false;
    for (const Statement& statement : plugin.manifest->statements) {
        if (statement.kind != Statement::Kind::Assignment || statement.expressions.empty()) {
            continue;
        }
        const Expr& value = statement.expressions.front();
        if (statement.name == "name") {
            has_name = value.kind == Expr::Kind::String && !value.token.text.empty();
        } else if (statement.name == "version") {
            has_version = value.kind == Expr::Kind::String && !value.token.text.empty();
        } else if (statement.name == "api_version") {
            has_api_version = value.kind == Expr::Kind::Integer;
            if (has_api_version && value.token.integer > supported_api_version) {
                errors.push_back("api_version " + std::to_string(value.token.integer) +
                                 " is newer than supported version " +
                                 std::to_string(supported_api_version));
            }
        }
    }
    // Two commands with the same name: the first wins and the second is
    // unreachable. Nothing about that is intentional, and nothing else in the
    // pipeline would report it.
    std::vector<std::string> seen;
    for (const Command& command : plugin.commands) {
        if (std::find(seen.begin(), seen.end(), command.name) != seen.end())
            errors.push_back("duplicate command '" + command.name + "'");
        else
            seen.push_back(command.name);
    }

    if (!has_name)
        errors.push_back("manifest requires a non-empty string 'name'");
    if (!has_version)
        errors.push_back("manifest requires a non-empty string 'version'");
    if (!has_api_version)
        errors.push_back("manifest requires an integer 'api_version'");
    return errors;
}

} // namespace kap::kpl
