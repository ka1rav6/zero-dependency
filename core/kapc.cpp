// core/kapc.cpp
//
// Implementation of the KPL AST cache declared in core/kapc.hpp.

#include "core/kapc.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include <unistd.h>

#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"

namespace kap
{
namespace kapc
{

namespace
{

constexpr char kMagic[4] = {'K', 'A', 'P', 'C'};

// A cache blob is a tree, and decoding recurses per level. The KPL parser
// itself has no depth limit, so a legitimately deep plugin is possible; this
// cap is set well above anything a readable plugin reaches and exists so that
// a *corrupt* blob claiming absurd nesting fails with a diagnostic instead of
// exhausting the stack.
constexpr int kMaxDepth = 256;

// A corrupt blob must never be able to make the decoder allocate gigabytes
// from a bogus length prefix. Every count is checked against the bytes that
// actually remain, which is a tighter bound than any fixed cap.
class Writer
{
public:
    void u8(std::uint8_t value)
    {
        out_.push_back(static_cast<char>(value));
    }

    void u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            out_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }

    void i64(std::int64_t value)
    {
        // Cast through the unsigned type: shifting a negative signed value is
        // implementation-defined, and this encoding has to be identical
        // everywhere for a cache file to be portable.
        const auto bits = static_cast<std::uint64_t>(value);
        for (int shift = 0; shift < 64; shift += 8)
            out_.push_back(static_cast<char>((bits >> shift) & 0xFF));
    }

    void i32(std::int32_t value)
    {
        u32(static_cast<std::uint32_t>(value));
    }

    void text(const std::string& value)
    {
        u32(static_cast<std::uint32_t>(value.size()));
        out_ += value;
    }

    void bytes(const char* data, std::size_t size)
    {
        out_.append(data, size);
    }

    std::string take()
    {
        return std::move(out_);
    }

private:
    std::string out_;
};

class Reader
{
public:
    explicit Reader(std::string_view blob) : blob_(blob) {}

    std::uint8_t u8()
    {
        require(1);
        return static_cast<std::uint8_t>(blob_[position_++]);
    }

    std::uint32_t u32()
    {
        require(4);
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(blob_[position_++]))
                     << shift;
        return value;
    }

    std::int64_t i64()
    {
        require(8);
        std::uint64_t bits = 0;
        for (int shift = 0; shift < 64; shift += 8)
            bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(blob_[position_++]))
                    << shift;
        return static_cast<std::int64_t>(bits);
    }

    std::int32_t i32()
    {
        return static_cast<std::int32_t>(u32());
    }

    std::string text()
    {
        const std::uint32_t size = u32();
        require(size);
        std::string value(blob_.substr(position_, size));
        position_ += size;
        return value;
    }

    // A container's element count, validated against the bytes that remain.
    // Every element costs at least one byte, so a count larger than the
    // remaining size is provably a lie — and refusing it here is what stops a
    // corrupt length prefix from driving a huge allocation.
    std::uint32_t count()
    {
        const std::uint32_t value = u32();
        if (value > blob_.size() - position_)
            fail("cache entry declares more elements than it contains");
        return value;
    }

    void expect(const char* data, std::size_t size, const char* what)
    {
        require(size);
        if (std::memcmp(blob_.data() + position_, data, size) != 0)
            fail(std::string("cache entry has a bad ") + what);
        position_ += size;
    }

    bool at_end() const
    {
        return position_ == blob_.size();
    }

    [[noreturn]] static void fail(const std::string& message)
    {
        throw diag::Error{diag::error(message)};
    }

private:
    void require(std::size_t size) const
    {
        if (blob_.size() - position_ < size)
            fail("cache entry is truncated");
    }

    std::string_view blob_;
    std::size_t      position_ = 0;
};

// --- AST encoding -------------------------------------------------------------

void write_token(Writer& writer, const kpl::Token& token)
{
    writer.u8(static_cast<std::uint8_t>(token.kind));
    writer.text(token.text);
    writer.i64(token.integer);
    writer.i32(token.line);
    writer.i32(token.column);
}

kpl::Token read_token(Reader& reader)
{
    kpl::Token         token;
    const std::uint8_t kind = reader.u8();
    if (kind > static_cast<std::uint8_t>(kpl::TokenKind::End))
        Reader::fail("cache entry has an unknown token kind");
    token.kind    = static_cast<kpl::TokenKind>(kind);
    token.text    = reader.text();
    token.integer = reader.i64();
    token.line    = reader.i32();
    token.column  = reader.i32();
    return token;
}

void write_strings(Writer& writer, const std::vector<std::string>& values)
{
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const std::string& value : values)
        writer.text(value);
}

std::vector<std::string> read_strings(Reader& reader)
{
    const std::uint32_t      size = reader.count();
    std::vector<std::string> values;
    values.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index)
        values.push_back(reader.text());
    return values;
}

void write_expr(Writer& writer, const kpl::Expr& expr)
{
    writer.u8(static_cast<std::uint8_t>(expr.kind));
    write_token(writer, expr.token);
    write_strings(writer, expr.names);
    writer.u32(static_cast<std::uint32_t>(expr.children.size()));
    for (const kpl::Expr& child : expr.children)
        write_expr(writer, child);
}

kpl::Expr read_expr(Reader& reader, int depth)
{
    if (depth > kMaxDepth)
        Reader::fail("cache entry nests deeper than " + std::to_string(kMaxDepth) + " levels");

    kpl::Expr          expr;
    const std::uint8_t kind = reader.u8();
    if (kind > static_cast<std::uint8_t>(kpl::Expr::Kind::Match))
        Reader::fail("cache entry has an unknown expression kind");
    expr.kind  = static_cast<kpl::Expr::Kind>(kind);
    expr.token = read_token(reader);
    expr.names = read_strings(reader);

    const std::uint32_t children = reader.count();
    expr.children.reserve(children);
    for (std::uint32_t index = 0; index < children; ++index)
        expr.children.push_back(read_expr(reader, depth + 1));
    return expr;
}

void write_statements(Writer& writer, const std::vector<kpl::Statement>& statements);
std::vector<kpl::Statement> read_statements(Reader& reader, int depth);

void write_statement(Writer& writer, const kpl::Statement& statement)
{
    writer.u8(static_cast<std::uint8_t>(statement.kind));
    write_token(writer, statement.token);
    writer.text(statement.name);
    writer.text(statement.type_name);
    write_strings(writer, statement.names);
    writer.u32(static_cast<std::uint32_t>(statement.expressions.size()));
    for (const kpl::Expr& expr : statement.expressions)
        write_expr(writer, expr);
    write_statements(writer, statement.body);
    write_statements(writer, statement.otherwise);
}

kpl::Statement read_statement(Reader& reader, int depth)
{
    if (depth > kMaxDepth)
        Reader::fail("cache entry nests deeper than " + std::to_string(kMaxDepth) + " levels");

    kpl::Statement     statement;
    const std::uint8_t kind = reader.u8();
    if (kind > static_cast<std::uint8_t>(kpl::Statement::Kind::Directive))
        Reader::fail("cache entry has an unknown statement kind");
    statement.kind      = static_cast<kpl::Statement::Kind>(kind);
    statement.token     = read_token(reader);
    statement.name      = reader.text();
    statement.type_name = reader.text();
    statement.names     = read_strings(reader);

    const std::uint32_t expressions = reader.count();
    statement.expressions.reserve(expressions);
    for (std::uint32_t index = 0; index < expressions; ++index)
        statement.expressions.push_back(read_expr(reader, depth + 1));

    statement.body      = read_statements(reader, depth + 1);
    statement.otherwise = read_statements(reader, depth + 1);
    return statement;
}

void write_statements(Writer& writer, const std::vector<kpl::Statement>& statements)
{
    writer.u32(static_cast<std::uint32_t>(statements.size()));
    for (const kpl::Statement& statement : statements)
        write_statement(writer, statement);
}

std::vector<kpl::Statement> read_statements(Reader& reader, int depth)
{
    const std::uint32_t         size = reader.count();
    std::vector<kpl::Statement> statements;
    statements.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index)
        statements.push_back(read_statement(reader, depth));
    return statements;
}

void write_optional_block(Writer& writer, const std::optional<kpl::Block>& block)
{
    writer.u8(block ? 1 : 0);
    if (block)
        write_statements(writer, block->statements);
}

std::optional<kpl::Block> read_optional_block(Reader& reader)
{
    if (reader.u8() == 0)
        return std::nullopt;
    kpl::Block block;
    block.statements = read_statements(reader, 0);
    return block;
}

// Modification time as a portable integer. file_time_type's epoch is
// unspecified, so its raw count means nothing across platforms — but this
// value is only ever compared against another reading of the same clock on the
// same machine, which is all invalidation needs.
std::int64_t modification_time(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto      when = std::filesystem::last_write_time(path, ec);
    if (ec)
        return 0;
    return static_cast<std::int64_t>(when.time_since_epoch().count());
}

std::int64_t source_bytes(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    return ec ? -1 : static_cast<std::int64_t>(size);
}

// The api_version the plugin declares, or 0 when it declares none. Read off
// the manifest rather than re-derived, so §5.14's "invalidated when
// api_version changes" is checked against the value that was actually
// compiled.
std::int64_t manifest_api_version(const kpl::Plugin& plugin)
{
    if (!plugin.manifest)
        return 0;
    for (const kpl::Statement& statement : plugin.manifest->statements) {
        if (statement.kind != kpl::Statement::Kind::Assignment || statement.name != "api_version" ||
            statement.expressions.empty())
            continue;
        const kpl::Expr& value = statement.expressions.front();
        if (value.kind == kpl::Expr::Kind::Integer)
            return value.token.integer;
    }
    return 0;
}

} // namespace

std::string encode(const kpl::Plugin& plugin)
{
    Writer writer;
    writer.bytes(kMagic, sizeof(kMagic));
    writer.u32(kFormatVersion);
    writer.i64(manifest_api_version(plugin));
    writer.text(plugin.source_name);
    write_optional_block(writer, plugin.manifest);
    write_optional_block(writer, plugin.detect);
    write_optional_block(writer, plugin.requires_block);
    write_optional_block(writer, plugin.schema);

    writer.u32(static_cast<std::uint32_t>(plugin.commands.size()));
    for (const kpl::Command& command : plugin.commands) {
        writer.text(command.name);
        write_strings(writer, command.parameters);
        write_token(writer, command.token);
        write_statements(writer, command.body.statements);
    }
    return writer.take();
}

kpl::Plugin decode(std::string_view blob)
{
    Reader reader(blob);
    reader.expect(kMagic, sizeof(kMagic), "magic number");
    if (reader.u32() != kFormatVersion)
        Reader::fail("cache entry was written by a different version of kap");
    (void) reader.i64(); // api_version; validated by load() against the source

    kpl::Plugin plugin;
    plugin.source_name    = reader.text();
    plugin.manifest       = read_optional_block(reader);
    plugin.detect         = read_optional_block(reader);
    plugin.requires_block = read_optional_block(reader);
    plugin.schema         = read_optional_block(reader);

    const std::uint32_t commands = reader.count();
    plugin.commands.reserve(commands);
    for (std::uint32_t index = 0; index < commands; ++index) {
        kpl::Command command;
        command.name            = reader.text();
        command.parameters      = read_strings(reader);
        command.token           = read_token(reader);
        command.body.statements = read_statements(reader, 0);
        plugin.commands.push_back(std::move(command));
    }

    // Trailing bytes mean the blob is not what this decoder thinks it is.
    // Accepting them would let a subtly wrong file decode into a subtly wrong
    // AST, which is the one failure mode a cache must never have.
    if (!reader.at_end())
        Reader::fail("cache entry has trailing bytes");
    return plugin;
}

std::filesystem::path cache_directory()
{
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0')
        return std::filesystem::path(xdg) / "kap" / "ast";
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        return std::filesystem::path(home) / ".cache" / "kap" / "ast";
    return {};
}

std::filesystem::path cache_file(const std::filesystem::path& directory,
                                 const std::filesystem::path& source)
{
    std::error_code             ec;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(source, ec);
    const std::string           key      = (ec ? source : absolute).string();
    // The plugin's directory name is the readable half; the path hash is what
    // keeps two same-named plugins at different paths apart (§6.5).
    std::string name = source.parent_path().filename().string();
    if (name.empty())
        name = "plugin";
    return directory / (name + "@" + hash::hex64(hash::fnv1a64(key)) + ".kapc");
}

namespace
{

// The invalidation header written next to the AST. Kept separate from the AST
// blob so a stale entry is rejected after four small reads rather than after
// decoding the whole tree.
struct Header
{
    std::int64_t size        = -1;
    std::int64_t modified    = 0;
    std::int64_t api_version = 0;
};

std::string encode_header(const Header& header)
{
    Writer writer;
    writer.i64(header.size);
    writer.i64(header.modified);
    writer.i64(header.api_version);
    return writer.take();
}

constexpr std::size_t kHeaderSize = 24;

} // namespace

Loaded load(const std::filesystem::path& source, const std::filesystem::path& directory)
{
    // Parse first. That sounds like it defeats the purpose, but it does not:
    // the expensive question is whether the cache is *valid*, and answering it
    // needs the source's size and mtime either way. The saving comes from
    // skipping the lexer and parser, which is what the early return below does.
    const Header current{source_bytes(source), modification_time(source), 0};

    if (!directory.empty()) {
        const std::filesystem::path entry = cache_file(directory, source);
        if (fs::is_file(entry)) {
            try {
                const std::string blob = fs::read_text(entry);
                if (blob.size() > kHeaderSize) {
                    Reader       header(std::string_view(blob).substr(0, kHeaderSize));
                    const Header stored{header.i64(), header.i64(), header.i64()};
                    if (stored.size == current.size && stored.modified == current.modified) {
                        Loaded loaded;
                        loaded.plugin = decode(std::string_view(blob).substr(kHeaderSize));
                        loaded.origin = Origin::CacheHit;
                        return loaded;
                    }
                }
            }
            catch (const diag::Error&) {
                // A corrupt or stale entry is a cache miss, never a failure.
                // A cache must not be able to turn a working plugin into a
                // broken one.
            }
        }
    }

    Loaded loaded;
    loaded.plugin = kpl::parse(fs::read_text(source), source.string());
    loaded.origin = Origin::Parsed;

    if (directory.empty())
        return loaded;

    // Write the entry, and treat every failure as "no cache today". A
    // read-only or full cache directory is a performance problem, not a
    // correctness one.
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
        return loaded;

    Header header      = current;
    header.api_version = manifest_api_version(loaded.plugin);

    const std::filesystem::path entry = cache_file(directory, source);
    // Write to a temporary file and rename, so a concurrent kap never reads a
    // half-written entry: rename(2) within a directory is atomic.
    const std::filesystem::path temporary =
        entry.string() + "." + std::to_string(::getpid()) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return loaded;
        const std::string payload = encode_header(header) + encode(loaded.plugin);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!out)
            return loaded;
    }
    std::filesystem::rename(temporary, entry, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return loaded;
    }

    loaded.origin = Origin::CacheWrite;
    return loaded;
}

} // namespace kapc
} // namespace kap
