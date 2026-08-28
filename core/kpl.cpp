// core/kpl.cpp
//
// KPL lexer. It is deliberately independent of the AST parser: tokenization
// errors can be tested and reported without constructing a plugin.

#include "core/kpl.hpp"

#include <cctype>
#include <charconv>
#include <algorithm>
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
                result.type_name += "<" + consume(TokenKind::Identifier, "expected list element type").text + ">";
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

bool schema_value_matches(const Value& value, std::string_view type,
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
        const Value::Kind element_kind = type == "list<str>" ? Value::Kind::String
                                                               : Value::Kind::Integer;
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
                case Expr::Kind::List: {
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
    std::map<std::string, Value> result;
    std::vector<std::string>     errors;
    const auto                   fields = schema(plugin);
    std::map<std::string, const SchemaField*> known;
    for (const SchemaField& field : fields) {
        if (!known.emplace(field.name, &field).second) {
            errors.push_back("duplicate schema field '" + field.name + "'");
            continue;
        }
        if (!field.default_value)
            errors.push_back("schema field '" + field.name + "' requires a default");
        else if (!schema_value_matches(*field.default_value, field.type, field.enum_values))
            errors.push_back("default for schema field '" + field.name + "' has type " + field.type);
        else
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
        case StaticType::ListUnknown:
            return "list";
        case StaticType::Record:
            return "record";
        case StaticType::Unknown:
            return "unknown";
    }
    return "unknown";
}

class TypeChecker
{
public:
    explicit TypeChecker(std::vector<std::string>& errors) : errors_(errors) {}

    void command(const Command& command)
    {
        values_.clear();
        values_["project"] = StaticType::Record;
        values_["config"]  = StaticType::Record;
        values_["extra"]   = StaticType::ListString;
        for (const std::string& parameter : command.parameters)
            values_.try_emplace(parameter, StaticType::Unknown);
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
            case Expr::Kind::Name: {
                const auto found = values_.find(expr.token.text);
                if (found == values_.end()) {
                    error("unknown name '" + expr.token.text + "'", expr.token);
                    return StaticType::Unknown;
                }
                return found->second;
            }
            case Expr::Kind::List: {
                bool all_strings = true;
                for (const Expr& child : expr.children)
                    all_strings = all_strings && expression(child) == StaticType::String;
                return all_strings ? StaticType::ListString : StaticType::ListUnknown;
            }
            case Expr::Kind::Record:
                for (const Expr& child : expr.children)
                    expression(child);
                return StaticType::Record;
            case Expr::Kind::Member: {
                const StaticType object = expression(expr.children.front());
                if (object != StaticType::Record && object != StaticType::Unknown)
                    error("member access requires a record", expr.token);
                const Expr& base = expr.children.front();
                if (base.kind == Expr::Kind::Name && base.token.text == "project") {
                    if (expr.token.text == "root")
                        return StaticType::String;
                    if (expr.token.text == "matched_files")
                        return StaticType::ListString;
                    error("unknown project member '" + expr.token.text + "'", expr.token);
                }
                return StaticType::Unknown;
            }
            case Expr::Kind::Index: {
                const StaticType object = expression(expr.children[0]);
                const StaticType index  = expression(expr.children[1]);
                if (index != StaticType::Integer && index != StaticType::Unknown)
                    error("list index must be an integer", expr.children[1].token);
                if (object == StaticType::ListString)
                    return StaticType::String;
                if (object != StaticType::ListUnknown && object != StaticType::Unknown)
                    error("index access requires a list", expr.token);
                return StaticType::Unknown;
            }
            case Expr::Kind::Unary: {
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
            case Expr::Kind::Conditional: {
                const StaticType condition = expression(expr.children[0]);
                if (condition != StaticType::Boolean && condition != StaticType::Unknown)
                    error("conditional condition must be a boolean", expr.children[0].token);
                const StaticType chosen = expression(expr.children[1]);
                const StaticType fallback = expression(expr.children[2]);
                return chosen == fallback ? chosen : StaticType::Unknown;
            }
            case Expr::Kind::Call:
                return call(expr);
        }
        return StaticType::Unknown;
    }

    StaticType binary(const Expr& expr)
    {
        const StaticType left  = expression(expr.children[0]);
        const StaticType right = expression(expr.children[1]);
        const TokenKind op     = expr.token.kind;
        if (op == TokenKind::AndAnd || op == TokenKind::OrOr) {
            require_boolean(left, expr.children[0].token);
            require_boolean(right, expr.children[1].token);
            return StaticType::Boolean;
        }
        if (op == TokenKind::EqualEqual || op == TokenKind::BangEqual)
            return StaticType::Boolean;
        if (op == TokenKind::Plus &&
            ((left == StaticType::String && right == StaticType::String) ||
             (left == StaticType::ListString && right == StaticType::ListString)))
            return left;
        if ((left == StaticType::Integer && right == StaticType::Integer) ||
            left == StaticType::Unknown || right == StaticType::Unknown)
            return op == TokenKind::Less || op == TokenKind::LessEqual ||
                           op == TokenKind::Greater || op == TokenKind::GreaterEqual
                       ? StaticType::Boolean
                       : StaticType::Integer;
        error("incompatible operands", expr.token);
        return StaticType::Unknown;
    }

    void require_boolean(StaticType type, const Token& token)
    {
        if (type != StaticType::Boolean && type != StaticType::Unknown)
            error("condition must be a boolean, got " + std::string(type_name(type)), token);
    }

    StaticType call(const Expr& expr)
    {
        const Expr& member = expr.children.front();
        if (member.kind != Expr::Kind::Member || member.children.front().kind != Expr::Kind::Name ||
            member.children.front().token.text != "project") {
            error("only project host calls are allowed", expr.token);
            return StaticType::Unknown;
        }
        if (expr.children.size() != 2 ||
            (expr.children.size() == 2 && expression(expr.children[1]) != StaticType::String))
            error("project host calls require one string argument", expr.token);
        if (member.token.text != "tool" && member.token.text != "exists")
            error("unknown project method '" + member.token.text + "'", member.token);
        return StaticType::Boolean;
    }

    void statement_check(const Statement& statement)
    {
        switch (statement.kind) {
            case Statement::Kind::Let:
            case Statement::Kind::Assignment:
                values_[statement.name] = expression(statement.expressions.front());
                return;
            case Statement::Kind::Step:
                for (const Expr& argument : statement.expressions) {
                    const StaticType type = expression(argument);
                    if (type != StaticType::String && type != StaticType::ListString &&
                        type != StaticType::Unknown)
                        error("step arguments must be strings, got " + std::string(type_name(type)),
                              argument.token);
                }
                if (statement.expressions.empty())
                    error("step requires a non-empty command", statement.token);
                return;
            case Statement::Kind::If:
                require_boolean(expression(statement.expressions.front()), statement.token);
                for (const Statement& child : statement.body)
                    statement_check(child);
                for (const Statement& child : statement.otherwise)
                    statement_check(child);
                return;
            case Statement::Kind::For:
                if (expression(statement.expressions.front()) == StaticType::None)
                    error("for loop requires a list", statement.expressions.front().token);
                values_[statement.name] = StaticType::Unknown;
                for (const Statement& child : statement.body)
                    statement_check(child);
                return;
            case Statement::Kind::Match:
                expression(statement.expressions.front());
                for (std::size_t index = 1; index < statement.expressions.size(); ++index)
                    expression(statement.expressions[index]);
                return;
            case Statement::Kind::Concurrent:
                require_boolean(expression(statement.expressions.front()), statement.token);
                return;
            case Statement::Kind::ReportFreedSpace:
            case Statement::Kind::Expression:
                for (const Expr& expr : statement.expressions)
                    expression(expr);
                return;
        }
    }

    std::vector<std::string>& errors_;
    std::map<std::string, StaticType> values_;
};

} // namespace

std::vector<std::string> type_check(const Plugin& plugin)
{
    std::vector<std::string> errors;
    for (const Command& command : plugin.commands)
        TypeChecker{errors}.command(command);
    return errors;
}

namespace
{

class Evaluator
{
public:
    Evaluator(const Project&                      project,
              const std::map<std::string, Value>& config,
              const std::vector<std::string>&     extra)
    {
        environment_["config"]  = Value::record_value(config);
        environment_["extra"]   = strings_to_value(extra);
        environment_["project"] = Value::record_value({
            {"root", Value::string_value(project.root)},
            {"matched_files", strings_to_value(project.matched_files)},
        });
        project_                = &project;
    }

    CommandSpec run(const Command& command)
    {
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

    static bool truthy(const Value& value)
    {
        return value.kind == Value::Kind::Boolean && value.boolean;
    }

    static std::string string(const Value& value, const Token& token, const Evaluator& evaluator)
    {
        if (value.kind != Value::Kind::String)
            evaluator.fail("step arguments must be strings", token);
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
                        return Value::boolean_value(!truthy(value));
                    if (value.kind != Value::Kind::Integer)
                        fail("unary '-' requires an integer", expr.token);
                    return Value::integer_value(-value.integer);
                }
            case Expr::Kind::Binary:
                return binary(expr);
            case Expr::Kind::Conditional:
                return truthy(expression(expr.children[0])) ? expression(expr.children[1])
                                                            : expression(expr.children[2]);
            case Expr::Kind::Call:
                return call(expr);
        }
        fail("unsupported expression", expr.token);
    }

    Value binary(const Expr& expr)
    {
        const Value left = expression(expr.children[0]);
        if (expr.token.kind == TokenKind::AndAnd && !truthy(left))
            return Value::boolean_value(false);
        if (expr.token.kind == TokenKind::OrOr && truthy(left))
            return Value::boolean_value(true);
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
            const bool equal = left.kind == right.kind && left.string == right.string &&
                               left.integer == right.integer && left.boolean == right.boolean;
            return Value::boolean_value(expr.token.kind == TokenKind::EqualEqual ? equal : !equal);
        }
        fail("incompatible operands", expr.token);
    }

    Value call(const Expr& expr)
    {
        const Expr& member = expr.children.front();
        if (member.kind != Expr::Kind::Member || member.children.front().token.text != "project")
            fail("only project host calls are allowed", expr.token);
        if (expr.children.size() != 2 || expr.children[1].kind != Expr::Kind::String)
            fail("project host calls require one string argument", expr.token);
        const std::string argument = expr.children[1].token.text;
        if (member.token.text == "tool")
            return Value::boolean_value(project_->tool && project_->tool(argument));
        if (member.token.text == "exists")
            return Value::boolean_value(project_->exists && project_->exists(argument));
        fail("unknown project method '" + member.token.text + "'", member.token);
    }

    void statement_run(const Statement& statement)
    {
        switch (statement.kind) {
            case Statement::Kind::Let:
            case Statement::Kind::Assignment:
                environment_[statement.name] = expression(statement.expressions.front());
                return;
            case Statement::Kind::Step:
                {
                    Step step;
                    for (const Expr& argument : statement.expressions) {
                        const Value value = expression(argument);
                        if (value.kind == Value::Kind::List) {
                            for (const Value& item : value.list)
                                step.command.push_back(string(item, argument.token, *this));
                        } else {
                            step.command.push_back(string(value, argument.token, *this));
                        }
                    }
                    if (step.command.empty())
                        fail("step requires a non-empty command", statement.token);
                    spec_.steps.push_back(std::move(step));
                    return;
                }
            case Statement::Kind::If:
                for (const Statement& child : truthy(expression(statement.expressions.front()))
                                                  ? statement.body
                                                  : statement.otherwise)
                    statement_run(child);
                return;
            case Statement::Kind::Concurrent:
                spec_.concurrent = truthy(expression(statement.expressions.front()));
                return;
            case Statement::Kind::ReportFreedSpace:
                spec_.report_freed_space = true;
                return;
            default:
                fail("statement is not supported by the evaluator yet", statement.token);
        }
    }

    const Project*               project_ = nullptr;
    std::string                  source_name_;
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
            return Evaluator{project, config, extra}.run(command);
    }
    throw diag::Error{diag::error("unknown command '" + std::string(command_name) + "'")};
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
    if (!has_name)
        errors.push_back("manifest requires a non-empty string 'name'");
    if (!has_version)
        errors.push_back("manifest requires a non-empty string 'version'");
    if (!has_api_version)
        errors.push_back("manifest requires an integer 'api_version'");
    return errors;
}

} // namespace kap::kpl
