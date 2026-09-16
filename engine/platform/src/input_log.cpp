#include <tynima/platform/input_log.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tynima::platform {

namespace {

constexpr const char* kMagic = "tynima-input-log";
constexpr int kVersion = 1;

// Bit strings as hex, most significant nibble first, one digit per four bits.
template <std::size_t N>
std::string to_hex(const std::bitset<N>& bits) {
    constexpr std::size_t digits = (N + 3) / 4;
    std::string out(digits, '0');
    for (std::size_t d = 0; d < digits; ++d) {
        unsigned nibble = 0;
        for (unsigned b = 0; b < 4; ++b) {
            const std::size_t bit = (digits - 1 - d) * 4 + b;
            if (bit < N && bits[bit]) {
                nibble |= 1u << b;
            }
        }
        out[d] = "0123456789abcdef"[nibble];
    }
    return out;
}

template <std::size_t N>
bool from_hex(const char* text, std::bitset<N>& bits) {
    constexpr std::size_t digits = (N + 3) / 4;
    if (std::strlen(text) != digits) {
        return false;
    }
    bits.reset();
    for (std::size_t d = 0; d < digits; ++d) {
        const char c = text[d];
        unsigned nibble;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<unsigned>(c - 'a' + 10);
        } else {
            return false;
        }
        for (unsigned b = 0; b < 4; ++b) {
            const std::size_t bit = (digits - 1 - d) * 4 + b;
            if ((nibble & (1u << b)) != 0u && bit < N) {
                bits[bit] = true;
            }
        }
    }
    return true;
}

// Floats as hex: every bit, no locale, no rounding.
void write_float(std::string& out, float f) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%a", static_cast<double>(f));
    out += buffer;
}

bool read_float(const char* text, float& f) {
    char* end = nullptr;
    const double d = std::strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    f = static_cast<float>(d);
    return true;
}

// "a,b,c,d" into up to `count` floats.
bool read_floats(const char* text, float* out, int count) {
    std::string piece;
    int n = 0;
    for (const char* p = text;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (n >= count || !read_float(piece.c_str(), out[n])) {
                return false;
            }
            ++n;
            piece.clear();
            if (*p == '\0') {
                break;
            }
        } else {
            piece += *p;
        }
    }
    return n == count;
}

bool failed(std::string& error, const char* path, int line, const char* what) {
    error = std::string(path) + ":" + std::to_string(line) + ": " + what;
    return false;
}

} // namespace

InputFrame capture_frame(const Input& input, float dt) noexcept {
    InputFrame frame;
    frame.dt = dt;
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        const auto key = static_cast<Key>(i);
        frame.keys_down[i] = input.key_down(key);
        frame.keys_pressed[i] = input.key_pressed(key);
        frame.keys_released[i] = input.key_released(key);
    }
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        const auto button = static_cast<MouseButton>(i);
        frame.buttons_down[i] = input.mouse_down(button);
        frame.buttons_pressed[i] = input.mouse_pressed(button);
        frame.buttons_released[i] = input.mouse_released(button);
    }
    frame.mouse_x = input.mouse_x();
    frame.mouse_y = input.mouse_y();
    frame.mouse_dx = input.mouse_dx();
    frame.mouse_dy = input.mouse_dy();
    frame.wheel_x = input.wheel_x();
    frame.wheel_y = input.wheel_y();
    return frame;
}

void apply_frame(const InputFrame& frame, Input& input) noexcept {
    Input::Writer::begin_frame(input);
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        const auto key = static_cast<Key>(i);
        const bool down = frame.keys_down[i];
        // Both edges in one frame: the order that leaves the recorded level.
        if (frame.keys_pressed[i] && frame.keys_released[i]) {
            Input::Writer::key(input, key, !down, false);
            Input::Writer::key(input, key, down, false);
        } else if (frame.keys_pressed[i]) {
            Input::Writer::key(input, key, true, false);
        } else if (frame.keys_released[i]) {
            Input::Writer::key(input, key, false, false);
        } else if (input.key_down(key) != down) {
            Input::Writer::key(input, key, down, true); // a level with no edge: as key repeat would
        }
    }
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        const auto button = static_cast<MouseButton>(i);
        const bool down = frame.buttons_down[i];
        if (frame.buttons_pressed[i] && frame.buttons_released[i]) {
            Input::Writer::mouse_button(input, button, !down);
            Input::Writer::mouse_button(input, button, down);
        } else if (frame.buttons_pressed[i]) {
            Input::Writer::mouse_button(input, button, true);
        } else if (frame.buttons_released[i]) {
            Input::Writer::mouse_button(input, button, false);
        }
    }
    Input::Writer::mouse_move(input, frame.mouse_x, frame.mouse_y, frame.mouse_dx, frame.mouse_dy);
    Input::Writer::wheel(input, frame.wheel_x, frame.wheel_y);
}

bool InputLog::save(const char* path, std::string& error) const {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        error = std::string("cannot write ") + path;
        return false;
    }
    std::string text;
    text += kMagic;
    text += " ";
    text += std::to_string(kVersion);
    text += "\nbackend ";
    text += backend;
    text += "\n";
    for (const InputFrame& frame : frames) {
        text += "frame ";
        write_float(text, frame.dt);
        if (frame.keys_down.any()) {
            text += " k=" + to_hex(frame.keys_down);
        }
        if (frame.keys_pressed.any()) {
            text += " p=" + to_hex(frame.keys_pressed);
        }
        if (frame.keys_released.any()) {
            text += " r=" + to_hex(frame.keys_released);
        }
        if (frame.buttons_down.any()) {
            text += " b=" + to_hex(frame.buttons_down);
        }
        if (frame.buttons_pressed.any()) {
            text += " bp=" + to_hex(frame.buttons_pressed);
        }
        if (frame.buttons_released.any()) {
            text += " br=" + to_hex(frame.buttons_released);
        }
        const bool mouse = frame.mouse_x != 0.0f || frame.mouse_y != 0.0f || frame.mouse_dx != 0.0f ||
                           frame.mouse_dy != 0.0f;
        if (mouse) {
            text += " m=";
            write_float(text, frame.mouse_x);
            text += ",";
            write_float(text, frame.mouse_y);
            text += ",";
            write_float(text, frame.mouse_dx);
            text += ",";
            write_float(text, frame.mouse_dy);
        }
        if (frame.wheel_x != 0.0f || frame.wheel_y != 0.0f) {
            text += " w=";
            write_float(text, frame.wheel_x);
            text += ",";
            write_float(text, frame.wheel_y);
        }
        text += "\n";
    }
    if (has_end) {
        char buffer[96];
        std::snprintf(buffer, sizeof buffer, "end frames=%zu steps=%" PRIu64 " hash=%016" PRIx64 "\n",
                      frames.size(), steps, state_hash);
        text += buffer;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fclose(file) != 0 || !ok) {
        error = std::string("cannot write ") + path;
        return false;
    }
    return true;
}

bool InputLog::load(const char* path, std::string& error) {
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

    backend.clear();
    frames.clear();
    steps = 0;
    state_hash = 0;
    has_end = false;
    int line_number = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(at, end - at);
        at = end + 1;
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        // Split on spaces.
        std::vector<std::string> words;
        for (std::size_t i = 0; i < line.size();) {
            const std::size_t space = line.find(' ', i);
            const std::size_t stop = space == std::string::npos ? line.size() : space;
            if (stop > i) {
                words.push_back(line.substr(i, stop - i));
            }
            i = stop + 1;
        }
        if (line_number == 1) {
            if (words.size() != 2 || words[0] != kMagic || std::atoi(words[1].c_str()) != kVersion) {
                return failed(error, path, line_number, "not a tynima input log (version 1)");
            }
            continue;
        }
        if (words[0] == "backend") {
            backend = words.size() > 1 ? words[1] : "";
        } else if (words[0] == "frame") {
            InputFrame frame;
            if (words.size() < 2 || !read_float(words[1].c_str(), frame.dt)) {
                return failed(error, path, line_number, "a frame needs its dt");
            }
            for (std::size_t w = 2; w < words.size(); ++w) {
                const std::string& word = words[w];
                const std::size_t eq = word.find('=');
                if (eq == std::string::npos) {
                    return failed(error, path, line_number, "expected field=value");
                }
                const std::string field = word.substr(0, eq);
                const char* value = word.c_str() + eq + 1;
                bool ok = true;
                if (field == "k") {
                    ok = from_hex(value, frame.keys_down);
                } else if (field == "p") {
                    ok = from_hex(value, frame.keys_pressed);
                } else if (field == "r") {
                    ok = from_hex(value, frame.keys_released);
                } else if (field == "b") {
                    ok = from_hex(value, frame.buttons_down);
                } else if (field == "bp") {
                    ok = from_hex(value, frame.buttons_pressed);
                } else if (field == "br") {
                    ok = from_hex(value, frame.buttons_released);
                } else if (field == "m") {
                    float m[4];
                    ok = read_floats(value, m, 4);
                    frame.mouse_x = m[0];
                    frame.mouse_y = m[1];
                    frame.mouse_dx = m[2];
                    frame.mouse_dy = m[3];
                } else if (field == "w") {
                    float wheel[2];
                    ok = read_floats(value, wheel, 2);
                    frame.wheel_x = wheel[0];
                    frame.wheel_y = wheel[1];
                } else {
                    ok = false;
                }
                if (!ok) {
                    return failed(error, path, line_number, "bad frame field");
                }
            }
            frames.push_back(frame);
        } else if (words[0] == "end") {
            std::size_t frame_count = 0;
            for (std::size_t w = 1; w < words.size(); ++w) {
                const std::string& word = words[w];
                if (word.rfind("frames=", 0) == 0) {
                    frame_count = static_cast<std::size_t>(std::strtoull(word.c_str() + 7, nullptr, 10));
                } else if (word.rfind("steps=", 0) == 0) {
                    steps = std::strtoull(word.c_str() + 6, nullptr, 10);
                } else if (word.rfind("hash=", 0) == 0) {
                    state_hash = std::strtoull(word.c_str() + 5, nullptr, 16);
                } else {
                    return failed(error, path, line_number, "bad end field");
                }
            }
            if (frame_count != frames.size()) {
                return failed(error, path, line_number, "frame count does not match the frames");
            }
            has_end = true;
        } else {
            return failed(error, path, line_number, "unexpected line");
        }
    }
    if (backend.empty()) {
        return failed(error, path, 0, "no backend line");
    }
    return true;
}

} // namespace tynima::platform
