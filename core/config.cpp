// core/config.cpp
//
// Implementation of the layered configuration declared in core/config.hpp
// (design doc §5.12, Milestone 6).

#include "core/config.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/paths.hpp"

namespace kap
{
namespace config
{

namespace
{

// Deep-merge `overlay` onto `base`. Tables are merged key by key; anything
// else replaces wholesale.
//
// Replacing rather than merging is the right rule for arrays, and it is worth
// being explicit about why: if `cmake_args` merged element-wise, a project
// could never *remove* an argument its user's global config had added, only
// add more. "The nearer layer wins outright" is the rule a user can predict.
void merge_into(toml::Value& base, const toml::Value& overlay)
{
    if (base.kind != toml::Value::Kind::Table || overlay.kind != toml::Value::Kind::Table) {
        base = overlay;
        return;
    }
    for (const auto& [key, value] : overlay.table) {
        auto found = base.table.find(key);
        if (found == base.table.end()) {
            base.table.emplace(key, value);
            continue;
        }
        merge_into(found->second, value);
    }
}

// Walk a dotted path through a table, returning nullptr when any component is
// missing or steps through a non-table.
const toml::Value* lookup(const toml::Value& table, const std::vector<std::string>& path)
{
    const toml::Value* current = &table;
    for (const std::string& component : path) {
        if (current->kind != toml::Value::Kind::Table)
            return nullptr;
        const auto found = current->table.find(component);
        if (found == current->table.end())
            return nullptr;
        current = &found->second;
    }
    return current;
}

std::vector<std::string> split_dotted(const std::string& key)
{
    std::vector<std::string> parts;
    std::size_t              start = 0;
    for (;;) {
        const std::size_t dot = key.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(key.substr(start));
            break;
        }
        parts.push_back(key.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

// Read one layer. A missing file is absent, not an error; a malformed one is
// fatal, because acting on a config we could not read would be a lie.
std::optional<Layer> read_layer(const std::string& name, const std::filesystem::path& file)
{
    if (file.empty() || !fs::is_file(file))
        return std::nullopt;
    Layer layer;
    layer.name  = name;
    layer.file  = file;
    layer.table = toml::parse(fs::read_text(file), file.string()).root();
    return layer;
}

// Pull kap's own settings out of the merged table.
Settings read_settings(const toml::Value& merged, std::vector<std::string>& warnings)
{
    Settings settings;

    if (const toml::Value* walk = lookup(merged, {"detect", "max_walk_up"}); walk != nullptr) {
        if (walk->kind == toml::Value::Kind::Integer && walk->integer >= 0)
            settings.max_walk_up = static_cast<int>(walk->integer);
        else
            warnings.push_back("[detect] max_walk_up must be a non-negative integer; ignoring it");
    }

    if (const toml::Value* pin = lookup(merged, {"detect", "ecosystem"}); pin != nullptr) {
        if (pin->kind == toml::Value::Kind::String)
            settings.ecosystem = pin->str;
        else
            warnings.push_back("[detect] ecosystem must be a string; ignoring it");
    }

    if (const toml::Value* hooks = lookup(merged, {"hooks"});
        hooks != nullptr && hooks->kind == toml::Value::Kind::Table) {
        for (const auto& [name, value] : hooks->table) {
            if (value.kind == toml::Value::Kind::String)
                settings.hooks.emplace(name, value.str);
            else
                warnings.push_back("[hooks] " + name +
                                   " must be a shell command string; "
                                   "ignoring it");
        }
    }

    return settings;
}

// --- TOML -> KPL value conversion --------------------------------------------------

// Convert one TOML value into the KPL value a config field holds. Returns
// nullopt for shapes a schema field can never have (a nested table, an array
// of mixed or non-scalar elements), which the caller reports as a bad key
// rather than silently dropping.
std::optional<kpl::Value> to_kpl(const toml::Value& value)
{
    switch (value.kind) {
        case toml::Value::Kind::String:
            return kpl::Value::string_value(value.str);
        case toml::Value::Kind::Integer:
            return kpl::Value::integer_value(value.integer);
        case toml::Value::Kind::Boolean:
            return kpl::Value::boolean_value(value.boolean);
        case toml::Value::Kind::Array:
            {
                std::vector<kpl::Value> items;
                items.reserve(value.array.size());
                for (const toml::Value& element : value.array) {
                    if (element.kind == toml::Value::Kind::String)
                        items.push_back(kpl::Value::string_value(element.str));
                    else if (element.kind == toml::Value::Kind::Integer)
                        items.push_back(kpl::Value::integer_value(element.integer));
                    else
                        return std::nullopt;
                }
                return kpl::Value::list_value(std::move(items));
            }
        case toml::Value::Kind::Table:
            return std::nullopt;
    }
    return std::nullopt;
}

// Split "a,b,c" for a list-typed --set. Comma rather than a repeated flag
// because §5.12's example is a single `--set key=value` pair, and a list-valued
// key still has to be expressible in one.
std::vector<std::string> split_commas(const std::string& text)
{
    std::vector<std::string> parts;
    if (text.empty())
        return parts; // --set cmake_args= means "empty list", not [""]
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = text.find(',', start);
        if (comma == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, comma - start));
        start = comma + 1;
    }
    return parts;
}

// Coerce a `--set` string to the type its schema field declares.
//
// The command line has no types — everything arrives as text — so the schema
// is the only thing that can say whether `release=true` means the string
// "true" or the boolean true. Without this, `--set release=true` would be
// rejected as "config key 'release' must be bool" while looking obviously
// correct to the person who typed it.
std::optional<kpl::Value> coerce(const std::string& text, const kpl::SchemaField& field)
{
    if (field.type == "bool") {
        if (text == "true")
            return kpl::Value::boolean_value(true);
        if (text == "false")
            return kpl::Value::boolean_value(false);
        return std::nullopt;
    }
    if (field.type == "int") {
        try {
            std::size_t     consumed = 0;
            const long long parsed   = std::stoll(text, &consumed);
            if (consumed != text.size())
                return std::nullopt;
            return kpl::Value::integer_value(static_cast<std::int64_t>(parsed));
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (field.type == "list<str>") {
        std::vector<kpl::Value> items;
        for (const std::string& part : split_commas(text))
            items.push_back(kpl::Value::string_value(part));
        return kpl::Value::list_value(std::move(items));
    }
    if (field.type == "list<int>") {
        std::vector<kpl::Value> items;
        for (const std::string& part : split_commas(text)) {
            try {
                std::size_t     consumed = 0;
                const long long parsed   = std::stoll(part, &consumed);
                if (consumed != part.size())
                    return std::nullopt;
                items.push_back(kpl::Value::integer_value(static_cast<std::int64_t>(parsed)));
            }
            catch (const std::exception&) {
                return std::nullopt;
            }
        }
        return kpl::Value::list_value(std::move(items));
    }
    // str and enum are both carried as strings; build_config checks an enum's
    // value against its declared members.
    return kpl::Value::string_value(text);
}

} // namespace

std::optional<std::string> Merged::hook(const std::string& phase, const std::string& command) const
{
    const auto found = settings.hooks.find(phase + "_" + command);
    if (found == settings.hooks.end())
        return std::nullopt;
    return found->second;
}

std::filesystem::path global_file()
{
    return paths::user_config_file();
}

std::filesystem::path project_file(const std::filesystem::path& root)
{
    return root / "kap.toml";
}

Merged load(const std::filesystem::path& root)
{
    Merged merged;

    // Lowest precedence first, so `layers` reads in the order §5.12 lists.
    if (auto global = read_layer("global", global_file()); global)
        merged.layers.push_back(std::move(*global));
    if (auto project = read_layer("project", project_file(root)); project)
        merged.layers.push_back(std::move(*project));

    merged.settings = read_settings(effective(merged), merged.warnings);
    return merged;
}

toml::Value effective(const Merged& merged)
{
    toml::Value combined = toml::make_table();
    for (const Layer& layer : merged.layers)
        merge_into(combined, layer.table);
    return combined;
}

PluginConfig for_plugin(const kpl::Plugin&              plugin,
                        const std::string&              plugin_name,
                        const Merged&                   merged,
                        const std::vector<std::string>& set_values)
{
    PluginConfig result;

    const std::vector<kpl::SchemaField> fields = kpl::schema(plugin);
    const auto find_field = [&fields](const std::string& name) -> const kpl::SchemaField* {
        for (const kpl::SchemaField& field : fields)
            if (field.name == name)
                return &field;
        return nullptr;
    };

    // Layers in order, so the project file overrides the global one.
    std::map<std::string, kpl::Value> overrides;
    for (const Layer& layer : merged.layers) {
        const toml::Value* section = lookup(layer.table, {"plugins", plugin_name});
        if (section == nullptr)
            continue;
        if (section->kind != toml::Value::Kind::Table) {
            result.errors.push_back("[plugins." + plugin_name + "] in " + layer.file.string() +
                                    " must be a table");
            continue;
        }
        for (const auto& [key, value] : section->table) {
            std::optional<kpl::Value> converted = to_kpl(value);
            if (!converted) {
                result.errors.push_back("config key 'plugins." + plugin_name + "." + key + "' in " +
                                        layer.file.string() +
                                        " has a shape no schema field can hold");
                continue;
            }
            overrides[key] = std::move(*converted);
        }
    }

    // --set last: it is the highest-precedence layer and applies to this
    // invocation only (§5.12).
    for (const std::string& assignment : set_values) {
        const std::size_t equals = assignment.find('=');
        if (equals == std::string::npos)
            continue; // the CLI parser already rejects these
        const std::string key   = assignment.substr(0, equals);
        const std::string value = assignment.substr(equals + 1);

        const kpl::SchemaField* field = find_field(key);
        if (field == nullptr) {
            // Reported by build_config below with the full "unknown config
            // key" message and the plugin's name attached, so nothing is added
            // here beyond making the key visible to it.
            overrides[key] = kpl::Value::string_value(value);
            continue;
        }
        std::optional<kpl::Value> coerced = coerce(value, *field);
        if (!coerced) {
            result.errors.push_back("--set " + key + "=" + value + ": '" + value +
                                    "' is not a valid " + field->type);
            continue;
        }
        overrides[key] = std::move(*coerced);
    }

    auto [values, errors] = kpl::build_config(plugin, overrides);
    result.values         = std::move(values);
    for (std::string& error : errors)
        result.errors.push_back(std::move(error));
    return result;
}

void set_key(const std::filesystem::path& file,
             const std::string&           dotted_key,
             const std::string&           value)
{
    toml::Value root = toml::make_table();
    if (fs::is_file(file)) {
        // A file we cannot parse must not be rewritten: `write` would emit our
        // (empty) understanding of it and destroy whatever was there.
        root = toml::parse(fs::read_text(file), file.string()).root();
    }

    const std::vector<std::string> path = split_dotted(dotted_key);
    if (path.empty() || path.back().empty()) {
        throw diag::Error{diag::error("'" + dotted_key + "' is not a usable configuration key")};
    }

    toml::Value* current = &root;
    for (std::size_t index = 0; index + 1 < path.size(); ++index) {
        auto [it, inserted] = current->table.emplace(path[index], toml::make_table());
        if (!inserted && it->second.kind != toml::Value::Kind::Table) {
            throw diag::Error{diag::error(
                "cannot set '" + dotted_key + "': '" + path[index] + "' is already a value",
                diag::Location{file.string()},
                {"remove or rename it first, or edit the file with 'kap config edit'"})};
        }
        current = &it->second;
    }

    // Type inference from the text, in the order that surprises least:
    // `true`/`false` are booleans, a pure integer is an integer, everything
    // else is a string. A user typing `kap config set detect.max_walk_up 3`
    // means the number, and storing "3" would fail the settings validation
    // with a message about a key they just set correctly.
    toml::Value stored;
    if (value == "true" || value == "false") {
        stored = toml::make_boolean(value == "true");
    } else {
        bool numeric = !value.empty();
        for (std::size_t index = 0; index < value.size(); ++index) {
            const char ch = value[index];
            if (ch == '-' && index == 0 && value.size() > 1)
                continue;
            if (ch < '0' || ch > '9') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            try {
                stored = toml::make_integer(static_cast<std::int64_t>(std::stoll(value)));
            }
            catch (const std::exception&) {
                stored = toml::make_string(value);
            }
        } else {
            stored = toml::make_string(value);
        }
    }
    current->table[path.back()] = std::move(stored);

    std::error_code ec;
    if (!file.parent_path().empty())
        std::filesystem::create_directories(file.parent_path(), ec);

    // Write-then-rename so an interrupted write cannot leave a truncated
    // config behind — losing a user's configuration to a full disk would be a
    // uniquely annoying way to fail.
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw diag::Error{
                diag::error("cannot write " + file.string(), diag::Location{file.string()})};
        }
        out << toml::write(root);
        if (!out) {
            throw diag::Error{
                diag::error("cannot write " + file.string(), diag::Location{file.string()})};
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw diag::Error{
            diag::error("cannot replace " + file.string(), diag::Location{file.string()})};
    }
}

} // namespace config
} // namespace kap
