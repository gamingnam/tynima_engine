#include <tynima/core/bytes.h>
#include <tynima/platform/file.h>
#include <tynima/sdk/project.h>

#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace tynima::sdk {

namespace {

constexpr bool is_absolute(std::string_view path) noexcept {
    return !path.empty() && (path[0] == '/' || path[0] == '\\' ||
                             (path.size() > 2 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')));
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// The directory part of a path, without its trailing slash; "." when there
// is none.
std::string directory_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return ".";
    }
    return std::string(path.substr(0, slash == 0 ? 1 : slash));
}

struct Reader {
    std::string_view text;
    std::string& error;
    Project& out;
    std::string section;
    int line_number = 0;

    [[nodiscard]] bool fail(const std::string& what) {
        error = "line " + std::to_string(line_number) + ": " + what;
        return false;
    }

    // A value: a quoted string, a number, or true/false. Comments after a
    // value are allowed, as they are in the scene format.
    [[nodiscard]] bool value(std::string_view raw, std::string& text_out, long long& number_out,
                             bool& bool_out, int& kind) {
        raw = trim(raw);
        if (raw.empty()) {
            return fail("a key with no value");
        }
        if (raw.front() == '"') {
            const std::size_t end = raw.find('"', 1);
            if (end == std::string_view::npos) {
                return fail("a string that never closes");
            }
            text_out = std::string(raw.substr(1, end - 1));
            kind = 0;
            const std::string_view rest = trim(raw.substr(end + 1));
            if (!rest.empty() && rest.front() != '#') {
                return fail("unexpected text after a string");
            }
            return true;
        }
        const std::size_t comment = raw.find('#');
        raw = trim(raw.substr(0, comment == std::string_view::npos ? raw.size() : comment));
        if (raw == "true" || raw == "false") {
            bool_out = raw == "true";
            kind = 2;
            return true;
        }
        long long parsed = 0;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) {
            return fail("a value that is not a string, a number or true/false");
        }
        number_out = parsed;
        kind = 1;
        return true;
    }

    [[nodiscard]] bool run() {
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = text.find('\n', start);
            std::string_view line =
                trim(text.substr(start, end == std::string_view::npos ? end : end - start));
            start = end == std::string_view::npos ? text.size() + 1 : end + 1;
            ++line_number;
            if (line.empty() || line.front() == '#') {
                continue;
            }
            if (line.front() == '[') {
                if (line.back() != ']') {
                    return fail("a section that never closes");
                }
                section = std::string(trim(line.substr(1, line.size() - 2)));
                if (section != "project" && section != "game" && section != "assets") {
                    return fail("no section called [" + section + "] (project, game, assets)");
                }
                continue;
            }
            const std::size_t equals = line.find('=');
            if (equals == std::string_view::npos) {
                return fail("a line that is neither a section nor a key = value");
            }
            const std::string key(trim(line.substr(0, equals)));
            std::string text_value;
            long long number_value = 0;
            bool bool_value = false;
            int kind = -1;
            if (!value(line.substr(equals + 1), text_value, number_value, bool_value, kind)) {
                return false;
            }
            if (!assign(key, text_value, number_value, kind)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool assign(const std::string& key, const std::string& text_value, long long number_value,
                              int kind) {
        const auto want_text = [&](std::string& field) {
            if (kind != 0) {
                return fail(key + " wants a string in quotes");
            }
            field = text_value;
            return true;
        };
        const auto want_number = [&](int& field) {
            if (kind != 1) {
                return fail(key + " wants a number");
            }
            field = static_cast<int>(number_value);
            return true;
        };
        if (section.empty()) {
            return fail("a key before any section");
        }
        if (section == "project" && key == "name") {
            return want_text(out.name);
        }
        if (section == "game") {
            if (key == "script") {
                return want_text(out.script);
            }
            if (key == "module") {
                return want_text(out.module);
            }
            if (key == "title") {
                return want_text(out.title);
            }
            if (key == "width") {
                return want_number(out.width);
            }
            if (key == "height") {
                return want_number(out.height);
            }
            if (key == "physics") {
                if (!want_text(out.physics)) {
                    return false;
                }
                if (out.physics != "tynima" && out.physics != "jolt") {
                    return fail("physics is \"tynima\" or \"jolt\", not \"" + out.physics + "\"");
                }
                return true;
            }
        }
        if (section == "assets") {
            if (key == "source") {
                return want_text(out.asset_dir);
            }
            if (key == "cooked") {
                return want_text(out.cooked_dir);
            }
        }
        // A key nobody knows is a typo, and a typo that is ignored is a
        // setting that quietly does nothing.
        return fail("[" + section + "] has no setting called " + key);
    }
};

} // namespace

std::string Project::resolve(const std::string& relative) const {
    if (relative.empty() || is_absolute(relative)) {
        return relative;
    }
    if (root.empty()) {
        return relative;
    }
    return root + "/" + relative;
}

bool load_project(const char* path, Project& out, std::string& error) {
    if (path == nullptr || path[0] == '\0') {
        error = "no project to read";
        return false;
    }
    std::string file = path;
    if (platform::directory_exists(file.c_str())) {
        file += std::string("/") + kProjectFileName;
    }
    // However the file was named — a doubled slash, a "." in the middle —
    // the project's root is the same string every time, so two tools that
    // found the same project agree about where its files are.
    file = std::filesystem::path(file).lexically_normal().generic_string();
    core::Bytes bytes;
    if (!platform::read_file(file.c_str(), bytes)) {
        error = "cannot read '" + file + "'";
        return false;
    }
    Project project;
    project.path = file;
    project.root = directory_of(file);
    Reader reader{.text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                  .error = error,
                  .out = project};
    if (!reader.run()) {
        error = file + ": " + error;
        return false;
    }
    if (project.script.empty() && project.module.empty()) {
        error = file + ": [game] has neither a script nor a module, so there is nothing to run";
        return false;
    }
    out = project;
    return true;
}

bool find_project(const char* start, Project& out, std::string& error) {
    // Absolute first: walking up from a relative path goes nowhere, since
    // every directory above "." is still ".".
    std::error_code ec;
    std::filesystem::path directory = std::filesystem::absolute(
        start != nullptr && start[0] != '\0' ? std::filesystem::path(start) : std::filesystem::path("."), ec);
    if (ec) {
        error = "cannot tell where this is: " + ec.message();
        return false;
    }
    directory = directory.lexically_normal();
    // Up to the root: a project is usually above where the command was run.
    for (int depth = 0; depth < 64; ++depth) {
        const std::filesystem::path candidate = directory / kProjectFileName;
        if (platform::file_write_time(candidate.string().c_str()) != 0) {
            return load_project(candidate.string().c_str(), out, error);
        }
        const std::filesystem::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) {
            break;
        }
        directory = parent;
    }
    error = std::string("no ") + kProjectFileName + " here or in any directory above";
    return false;
}

std::string write_project_text(const Project& project) {
    const auto quote = [](const std::string& text) { return "\"" + text + "\""; };
    std::string out = "# Tynima project\n[project]\nname = ";
    out += quote(project.name) + "\n\n[game]\n";
    out += "script = " + quote(project.script) + "\n";
    out += "module = " + quote(project.module) + "\n";
    out += "title = " + quote(project.title) + "\n";
    out += "width = " + std::to_string(project.width) + "\n";
    out += "height = " + std::to_string(project.height) + "\n";
    out += "physics = " + quote(project.physics) + "\n";
    out += "\n[assets]\nsource = " + quote(project.asset_dir) + "\n";
    out += "cooked = " + quote(project.cooked_dir) + "\n";
    return out;
}

bool save_project(const Project& project, const char* path, std::string& error) {
    const std::string text = write_project_text(project);
    if (!platform::write_file(path, text.data(), text.size())) {
        error = std::string("cannot write '") + path + "'";
        return false;
    }
    return true;
}

} // namespace tynima::sdk
