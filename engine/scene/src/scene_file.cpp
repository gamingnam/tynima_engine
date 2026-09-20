#include <tynima/scene/scene_file.h>

#include <tynima/core/reflect.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace tynima::scene {

using core::FieldInfo;
using core::FieldKind;

namespace {

// ------------------------------------------------------------------ writing

bool is_bare_key(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// A TOML basic string: quotes, backslashes and control characters escaped.
void write_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
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
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04X",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write_key(std::string& out, std::string_view key) {
    if (is_bare_key(key)) {
        out += key;
    } else {
        write_string(out, key);
    }
}

// The fewest digits that read back to the same value, and always a float
// to TOML's eye (a point or an exponent, or nan/inf).
template <typename T> void write_float(std::string& out, T value) {
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    out += text;
    if (text.find_first_of(".eEn") == std::string_view::npos) {
        out += ".0";
    }
}

void write_floats(std::string& out, const float* values, std::uint32_t count) {
    out += '[';
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i > 0) {
            out += ", ";
        }
        write_float(out, values[i]);
    }
    out += ']';
}

template <typename T> T read_as(const void* data) noexcept {
    T value;
    std::memcpy(&value, data, sizeof value);
    return value;
}

// The number an entity goes by in the file: its position among the saved
// ones, or -1 for one that is not there (dead, or its slot reused since).
struct FileIds {
    std::vector<Entity> entity_at_index;
    std::vector<std::int64_t> id_at_index;

    [[nodiscard]] std::int64_t of(Entity entity) const noexcept {
        if (!entity || entity.index >= entity_at_index.size() || entity_at_index[entity.index] != entity) {
            return -1;
        }
        return id_at_index[entity.index];
    }
};

// One field's value, as text; false for a kind that is never saved.
bool write_value(std::string& out, const FieldInfo& field, const void* data, const FileIds& file_ids) {
    char text[32];
    switch (field.kind) {
    case FieldKind::Bool:
        out += read_as<bool>(data) ? "true" : "false";
        return true;
    case FieldKind::Int8:
        std::snprintf(text, sizeof text, "%d", int{read_as<std::int8_t>(data)});
        break;
    case FieldKind::Uint8:
        std::snprintf(text, sizeof text, "%u", unsigned{read_as<std::uint8_t>(data)});
        break;
    case FieldKind::Int16:
        std::snprintf(text, sizeof text, "%d", int{read_as<std::int16_t>(data)});
        break;
    case FieldKind::Uint16:
        std::snprintf(text, sizeof text, "%u", unsigned{read_as<std::uint16_t>(data)});
        break;
    case FieldKind::Int32:
        std::snprintf(text, sizeof text, "%d", read_as<std::int32_t>(data));
        break;
    case FieldKind::Uint32:
        std::snprintf(text, sizeof text, "%u", read_as<std::uint32_t>(data));
        break;
    case FieldKind::Int64:
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(read_as<std::int64_t>(data)));
        break;
    case FieldKind::Uint64:
        std::snprintf(text, sizeof text, "%llu",
                      static_cast<unsigned long long>(read_as<std::uint64_t>(data)));
        break;
    case FieldKind::Float:
        write_float(out, read_as<float>(data));
        return true;
    case FieldKind::Double:
        write_float(out, read_as<double>(data));
        return true;
    case FieldKind::Vec2:
        write_floats(out, static_cast<const float*>(data), 2);
        return true;
    case FieldKind::Vec3:
        write_floats(out, static_cast<const float*>(data), 3);
        return true;
    case FieldKind::Vec4:
    case FieldKind::Quat:
        write_floats(out, static_cast<const float*>(data), 4);
        return true;
    case FieldKind::Mat4:
        write_floats(out, static_cast<const float*>(data), 16);
        return true;
    case FieldKind::Entity:
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(file_ids.of(read_as<Entity>(data))));
        break;
    case FieldKind::String: {
        const auto* chars = static_cast<const char*>(data);
        const std::size_t length = ::strnlen(chars, field.count);
        write_string(out, std::string_view(chars, length));
        return true;
    }
    case FieldKind::Bytes: {
        static constexpr char kHex[] = "0123456789abcdef";
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        out += '"';
        for (std::uint32_t i = 0; i < field.size; ++i) {
            out += kHex[bytes[i] >> 4];
            out += kHex[bytes[i] & 15];
        }
        out += '"';
        return true;
    }
    case FieldKind::Handle:
        return false;
    }
    out += text;
    return true;
}

bool is_saved(const FieldInfo& field) noexcept {
    return (field.flags & (core::kFieldReadOnly | core::kFieldHidden)) == 0 &&
           field.kind != FieldKind::Handle;
}

// Every live entity, by index.
std::vector<Entity> all_entities(World& world) {
    std::vector<Entity> entities;
    world.each_chunk_raw(
        nullptr, 0,
        [](void* user, const ChunkView& view) {
            auto* out = static_cast<std::vector<Entity>*>(user);
            out->insert(out->end(), view.entities, view.entities + view.count);
        },
        &entities);
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return entities;
}

// ------------------------------------------------------------------ reading

struct Value {
    enum class Type { String, Integer, Float, Bool, Array } type = Type::Integer;
    std::string text;         // String
    std::int64_t integer = 0; // Integer (also read as a float when a float is wanted)
    double number = 0.0;      // Float
    bool boolean = false;
    std::vector<double> array; // numbers only: what the writer produces
    int line = 0;
};

struct ComponentRecord {
    std::string name;
    int line = 0;
    std::vector<std::pair<std::string, Value>> fields;
};

struct EntityRecord {
    int line = 0;
    std::vector<ComponentRecord> components;
};

struct Parser {
    std::string_view text;
    std::size_t at = 0;
    int line = 1;
    std::string& error;

    explicit Parser(std::string_view t, std::string& e) : text(t), error(e) {}

    bool fail(const std::string& message, int at_line) {
        error = "line " + std::to_string(at_line) + ": " + message;
        return false;
    }
    [[nodiscard]] bool done() const noexcept { return at >= text.size(); }
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return at + ahead < text.size() ? text[at + ahead] : '\0';
    }
    void skip_blanks() noexcept {
        while (!done() && (peek() == ' ' || peek() == '\t')) {
            ++at;
        }
    }
    // Blanks, a comment, and the end of the line; false for anything else there.
    bool end_of_line() {
        skip_blanks();
        if (peek() == '#') {
            while (!done() && peek() != '\n') {
                ++at;
            }
        }
        if (done()) {
            return true;
        }
        if (peek() == '\r') {
            ++at;
        }
        if (peek() != '\n') {
            return fail("unexpected text after the value", line);
        }
        ++at;
        ++line;
        return true;
    }

    bool read_basic_string(std::string& out) {
        // At the opening quote.
        ++at;
        while (!done()) {
            const char c = peek();
            ++at;
            if (c == '"') {
                return true;
            }
            if (c == '\n') {
                break;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            const char e = peek();
            ++at;
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = peek();
                    ++at;
                    unsigned digit;
                    if (h >= '0' && h <= '9') {
                        digit = static_cast<unsigned>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        digit = static_cast<unsigned>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        digit = static_cast<unsigned>(h - 'A' + 10);
                    } else {
                        return fail("a \\u escape needs four hex digits", line);
                    }
                    code = code * 16 + digit;
                }
                // UTF-8, the three sizes a four-digit escape can need.
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default:
                return fail("unknown escape in a string", line);
            }
        }
        return fail("a string that never closes", line);
    }

    // A bare key, or a quoted one.
    bool read_key(std::string& out) {
        if (peek() == '"') {
            return read_basic_string(out);
        }
        const std::size_t start = at;
        while (!done() && is_bare_key(text.substr(at, 1))) {
            ++at;
        }
        if (at == start) {
            return fail("expected a key", line);
        }
        out.assign(text.substr(start, at - start));
        return true;
    }

    bool read_number(Value& value) {
        const std::size_t start = at;
        while (!done() && (peek() == '+' || peek() == '-' || peek() == '.' || peek() == '_' ||
                           (peek() >= '0' && peek() <= '9') || (peek() >= 'a' && peek() <= 'z') ||
                           (peek() >= 'A' && peek() <= 'Z'))) {
            ++at;
        }
        std::string token(text.substr(start, at - start));
        token.erase(std::remove(token.begin(), token.end(), '_'), token.end()); // TOML allows 1_000
        if (token.empty()) {
            return fail("expected a value", line);
        }
        const bool is_float = token.find_first_of(".eE") != std::string::npos || token == "inf" ||
                              token == "+inf" || token == "-inf" || token == "nan" || token == "+nan" ||
                              token == "-nan";
        char* end = nullptr;
        if (is_float) {
            value.type = Value::Type::Float;
            value.number = std::strtod(token.c_str(), &end);
        } else {
            value.type = Value::Type::Integer;
            value.integer = std::strtoll(token.c_str(), &end, 10);
            value.number = static_cast<double>(value.integer);
        }
        if (end == nullptr || *end != '\0') {
            return fail("not a number: " + token, line);
        }
        return true;
    }

    bool read_value(Value& value) {
        value.line = line;
        skip_blanks();
        const char c = peek();
        if (c == '"') {
            value.type = Value::Type::String;
            return read_basic_string(value.text);
        }
        if (c == '[') {
            ++at;
            value.type = Value::Type::Array;
            for (;;) {
                skip_blanks();
                if (peek() == ']') {
                    ++at;
                    return true;
                }
                Value element;
                if (!read_number(element)) {
                    return false;
                }
                value.array.push_back(element.number);
                skip_blanks();
                if (peek() == ',') {
                    ++at;
                } else if (peek() != ']') {
                    return fail("expected ',' or ']' in an array", line);
                }
            }
        }
        if (text.substr(at, 4) == "true" && !is_bare_key(text.substr(at + 4, 1))) {
            at += 4;
            value.type = Value::Type::Bool;
            value.boolean = true;
            return true;
        }
        if (text.substr(at, 5) == "false" && !is_bare_key(text.substr(at + 5, 1))) {
            at += 5;
            value.type = Value::Type::Bool;
            value.boolean = false;
            return true;
        }
        return read_number(value);
    }

    // The whole file into records. Top-level keys go to `header`.
    bool parse(std::vector<EntityRecord>& entities, std::vector<std::pair<std::string, Value>>& header) {
        EntityRecord* entity = nullptr;
        ComponentRecord* component = nullptr;
        while (!done()) {
            skip_blanks();
            const char c = peek();
            if (c == '\n' || c == '\r' || c == '#' || c == '\0') {
                if (!end_of_line()) {
                    return false;
                }
                continue;
            }
            if (c == '[') {
                const bool array = peek(1) == '[';
                at += array ? 2 : 1;
                skip_blanks();
                std::string first;
                if (!read_key(first)) {
                    return false;
                }
                skip_blanks();
                if (array) {
                    if (first != "entities" || peek() != ']' || peek(1) != ']') {
                        return fail("the only array of tables is [[entities]]", line);
                    }
                    at += 2;
                    entities.push_back(EntityRecord{.line = line});
                    entity = &entities.back();
                    component = nullptr;
                } else {
                    if (first != "entities" || peek() != '.') {
                        return fail("the only tables are [entities.<Component>]", line);
                    }
                    ++at;
                    std::string name;
                    if (!read_key(name)) {
                        return false;
                    }
                    skip_blanks();
                    if (peek() != ']') {
                        return fail("expected ']' after the table name", line);
                    }
                    ++at;
                    if (entity == nullptr) {
                        return fail("a component table before any [[entities]]", line);
                    }
                    entity->components.push_back(ComponentRecord{.name = name, .line = line});
                    component = &entity->components.back();
                }
                if (!end_of_line()) {
                    return false;
                }
                continue;
            }
            std::string key;
            if (!read_key(key)) {
                return false;
            }
            skip_blanks();
            if (peek() != '=') {
                return fail("expected '=' after the key", line);
            }
            ++at;
            Value value;
            if (!read_value(value)) {
                return false;
            }
            if (!end_of_line()) {
                return false;
            }
            if (component != nullptr) {
                component->fields.emplace_back(std::move(key), std::move(value));
            } else if (entity != nullptr) {
                return fail("a key directly under [[entities]]: fields go in a component table", value.line);
            } else {
                header.emplace_back(std::move(key), std::move(value));
            }
        }
        return true;
    }
};

// An entity-valued field to fill in once every entity of the file exists.
struct Fixup {
    std::uint32_t component; // index into the entity's component list
    std::uint32_t offset;    // of the field
    std::int64_t target;     // the file's number for the entity
};

// A parsed value into a field's bytes; false with `error` when it does not
// fit. An entity number is kept for later rather than stored.
bool store_value(const FieldInfo& field, const Value& value, std::uint8_t* data, std::string& error,
                 std::int64_t& entity_target, bool& is_entity) {
    const auto fail = [&](const char* wanted) {
        error = "line " + std::to_string(value.line) + ": " + field.name + " wants " + wanted;
        return false;
    };
    const auto store_integer = [&](auto typed) {
        using T = decltype(typed);
        if (value.type != Value::Type::Integer) {
            return fail("an integer");
        }
        const T converted = static_cast<T>(value.integer);
        if (static_cast<std::int64_t>(converted) != value.integer && !(std::is_same_v<T, std::uint64_t>)) {
            return fail("an integer that fits");
        }
        std::memcpy(data, &converted, sizeof converted);
        return true;
    };
    const auto store_floats = [&](std::uint32_t count) {
        if (value.type != Value::Type::Array || value.array.size() != count) {
            char wanted[32];
            std::snprintf(wanted, sizeof wanted, "%u numbers", count);
            return fail(wanted);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(value.array[i]);
            std::memcpy(data + i * sizeof(float), &f, sizeof f);
        }
        return true;
    };
    switch (field.kind) {
    case FieldKind::Bool:
        if (value.type != Value::Type::Bool) {
            return fail("true or false");
        }
        data[0] = value.boolean ? 1 : 0;
        return true;
    case FieldKind::Int8:
        return store_integer(std::int8_t{});
    case FieldKind::Uint8:
        return store_integer(std::uint8_t{});
    case FieldKind::Int16:
        return store_integer(std::int16_t{});
    case FieldKind::Uint16:
        return store_integer(std::uint16_t{});
    case FieldKind::Int32:
        return store_integer(std::int32_t{});
    case FieldKind::Uint32:
        return store_integer(std::uint32_t{});
    case FieldKind::Int64:
        return store_integer(std::int64_t{});
    case FieldKind::Uint64:
        return store_integer(std::uint64_t{});
    case FieldKind::Float:
    case FieldKind::Double: {
        if (value.type != Value::Type::Float && value.type != Value::Type::Integer) {
            return fail("a number");
        }
        if (field.kind == FieldKind::Float) {
            const auto f = static_cast<float>(value.number);
            std::memcpy(data, &f, sizeof f);
        } else {
            std::memcpy(data, &value.number, sizeof value.number);
        }
        return true;
    }
    case FieldKind::Vec2:
        return store_floats(2);
    case FieldKind::Vec3:
        return store_floats(3);
    case FieldKind::Vec4:
    case FieldKind::Quat:
        return store_floats(4);
    case FieldKind::Mat4:
        return store_floats(16);
    case FieldKind::Entity:
        if (value.type != Value::Type::Integer) {
            return fail("an entity number");
        }
        entity_target = value.integer;
        is_entity = true;
        return true;
    case FieldKind::String: {
        if (value.type != Value::Type::String) {
            return fail("a string");
        }
        if (value.text.size() >= field.count) {
            return fail("a shorter string");
        }
        std::memset(data, 0, field.count);
        std::memcpy(data, value.text.data(), value.text.size());
        return true;
    }
    case FieldKind::Bytes: {
        if (value.type != Value::Type::String || value.text.size() != std::size_t{field.size} * 2) {
            return fail("a hex string of its size");
        }
        for (std::uint32_t i = 0; i < field.size; ++i) {
            const auto digit = [](char h) -> int {
                if (h >= '0' && h <= '9')
                    return h - '0';
                if (h >= 'a' && h <= 'f')
                    return h - 'a' + 10;
                if (h >= 'A' && h <= 'F')
                    return h - 'A' + 10;
                return -1;
            };
            const int hi = digit(value.text[i * 2]), lo = digit(value.text[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return fail("a hex string");
            }
            data[i] = static_cast<std::uint8_t>(hi * 16 + lo);
        }
        return true;
    }
    case FieldKind::Handle:
        return fail("nothing: handles are not saved");
    }
    return false;
}

} // namespace

std::string save_scene_text(World& world) {
    std::string out = "# Tynima scene\nversion = " + std::to_string(kSceneFileVersion) + "\n";
    const std::vector<Entity> entities = all_entities(world);
    FileIds file_ids;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].index >= file_ids.id_at_index.size()) {
            file_ids.id_at_index.resize(entities[i].index + 1, -1);
            file_ids.entity_at_index.resize(entities[i].index + 1);
        }
        file_ids.id_at_index[entities[i].index] = static_cast<std::int64_t>(i);
        file_ids.entity_at_index[entities[i].index] = entities[i];
    }
    ComponentId ids[kMaxComponentTypes];
    for (const Entity entity : entities) {
        out += "\n[[entities]]\n";
        const std::uint32_t count =
            std::min(world.entity_components(entity, ids, kMaxComponentTypes), kMaxComponentTypes);
        for (std::uint32_t c = 0; c < count; ++c) {
            const ComponentInfo& info = world.component_info(ids[c]);
            const auto* data = static_cast<const std::uint8_t*>(world.get_raw_public(entity, ids[c]));
            out += "[entities.";
            write_key(out, info.name);
            out += "]\n";
            if (data == nullptr) {
                continue;
            }
            for (std::uint32_t f = 0; f < info.field_count; ++f) {
                const FieldInfo& field = info.fields[f];
                if (!is_saved(field)) {
                    continue;
                }
                std::string line;
                write_key(line, field.name);
                line += " = ";
                if (write_value(line, field, data + field.offset, file_ids)) {
                    out += line;
                    out += '\n';
                }
            }
        }
    }
    return out;
}

bool save_scene_file(World& world, const char* path, std::string& error) {
    const std::string text = save_scene_text(world);
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        error = std::string("cannot write ") + path;
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    if (!ok) {
        error = std::string("cannot write ") + path;
    }
    return ok;
}

bool load_scene_text(World& world, std::string_view text, std::string& error, const LoadOptions& options) {
    std::vector<EntityRecord> records;
    std::vector<std::pair<std::string, Value>> header;
    Parser parser(text, error);
    if (!parser.parse(records, header)) {
        return false;
    }
    for (const auto& [key, value] : header) {
        if (key == "version") {
            if (value.type != Value::Type::Integer || value.integer < 1 ||
                value.integer > static_cast<std::int64_t>(kSceneFileVersion)) {
                error =
                    "line " + std::to_string(value.line) + ": a scene file version this engine cannot read";
                return false;
            }
        } else {
            error = "line " + std::to_string(value.line) + ": unknown key " + key;
            return false;
        }
    }

    // Everything checked and converted first, so a bad file changes nothing.
    struct Prepared {
        std::vector<ComponentId> ids;
        std::vector<std::vector<std::uint8_t>> values;
        std::vector<Fixup> fixups;
    };
    std::vector<Prepared> prepared(records.size());
    for (std::size_t e = 0; e < records.size(); ++e) {
        Prepared& p = prepared[e];
        for (const ComponentRecord& component : records[e].components) {
            const ComponentId id = world.find_component(component.name.c_str());
            if (id == World::kNoComponent) {
                error = "line " + std::to_string(component.line) + ": component " + component.name +
                        " is not registered (load the module that defines it first)";
                return false;
            }
            if (std::find(p.ids.begin(), p.ids.end(), id) != p.ids.end()) {
                error =
                    "line " + std::to_string(component.line) + ": " + component.name + " twice on one entity";
                return false;
            }
            const ComponentInfo& info = world.component_info(id);
            std::vector<std::uint8_t> bytes(info.size, 0);
            if (info.defaults != nullptr) {
                std::memcpy(bytes.data(), info.defaults, info.size);
            }
            for (const auto& [key, value] : component.fields) {
                const FieldInfo* field = nullptr;
                for (std::uint32_t f = 0; f < info.field_count; ++f) {
                    if (key == info.fields[f].name) {
                        field = &info.fields[f];
                        break;
                    }
                }
                if (field == nullptr || !is_saved(*field)) {
                    error = "line " + std::to_string(value.line) + ": " + component.name + " has no field " +
                            key + " to load";
                    return false;
                }
                std::int64_t target = -1;
                bool is_entity = false;
                if (!store_value(*field, value, bytes.data() + field->offset, error, target, is_entity)) {
                    return false;
                }
                if (is_entity) {
                    p.fixups.push_back(Fixup{.component = static_cast<std::uint32_t>(p.ids.size()),
                                             .offset = field->offset,
                                             .target = target});
                }
            }
            p.ids.push_back(id);
            p.values.push_back(std::move(bytes));
        }
    }

    if (options.clear) {
        for (const Entity entity : all_entities(world)) {
            (void)world.destroy(entity);
        }
    }
    std::vector<Entity> created;
    created.reserve(records.size());
    for (const Prepared& p : prepared) {
        std::vector<const void*> values;
        values.reserve(p.values.size());
        for (const std::vector<std::uint8_t>& bytes : p.values) {
            values.push_back(bytes.data());
        }
        created.push_back(
            world.create_raw_public(p.ids.data(), values.data(), static_cast<std::uint32_t>(p.ids.size())));
    }
    // Entity fields: the file's numbers into the handles just made.
    for (std::size_t e = 0; e < prepared.size(); ++e) {
        for (const Fixup& fixup : prepared[e].fixups) {
            Entity value; // null unless the number names an entity of the file
            if (fixup.target >= 0 && static_cast<std::size_t>(fixup.target) < created.size()) {
                value = created[static_cast<std::size_t>(fixup.target)];
            }
            auto* data = static_cast<std::uint8_t*>(
                world.get_raw_public(created[e], prepared[e].ids[fixup.component]));
            if (data != nullptr) {
                std::memcpy(data + fixup.offset, &value, sizeof value);
            }
        }
    }
    return true;
}

bool load_scene_file(World& world, const char* path, std::string& error, const LoadOptions& options) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("cannot read ") + path;
        return false;
    }
    std::string text;
    char chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
        text.append(chunk, n);
    }
    std::fclose(file);
    if (!load_scene_text(world, text, error, options)) {
        error = std::string(path) + ": " + error;
        return false;
    }
    return true;
}

} // namespace tynima::scene
