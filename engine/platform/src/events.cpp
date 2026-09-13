#include <tynima/platform/events.h>

#include <tynima/core/profile.h>
#include <tynima/platform/input.h>

#include "sdl.h"

namespace tynima::platform {

namespace {

bool mouse_button_from_sdl(Uint8 sdl_button, MouseButton& out) noexcept {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: out = MouseButton::Left; return true;
    case SDL_BUTTON_MIDDLE: out = MouseButton::Middle; return true;
    case SDL_BUTTON_RIGHT: out = MouseButton::Right; return true;
    case SDL_BUTTON_X1: out = MouseButton::X1; return true;
    case SDL_BUTTON_X2: out = MouseButton::X2; return true;
    default: return false;
    }
}

Event window_event(EventType type, const SDL_WindowEvent& w) noexcept {
    Event event{};
    event.type = type;
    event.window_id = w.windowID;
    return event;
}

} // namespace

void pump_events(Input& input, std::vector<Event>& events) {
    TY_PROFILE_SCOPE_NAMED("platform::pump_events");
    events.clear();
    Input::Writer::begin_frame(input);

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: {
            Event event{};
            event.type = EventType::Quit;
            events.push_back(event);
            break;
        }
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            events.push_back(window_event(EventType::WindowClose, e.window));
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            events.push_back(window_event(EventType::WindowFocusGained, e.window));
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            events.push_back(window_event(EventType::WindowFocusLost, e.window));
            break;
        case SDL_EVENT_WINDOW_RESIZED: {
            Event event = window_event(EventType::WindowResized, e.window);
            event.width = e.window.data1;
            event.height = e.window.data2;
            if (SDL_Window* window = SDL_GetWindowFromID(e.window.windowID)) {
                SDL_GetWindowSizeInPixels(window, &event.pixel_width, &event.pixel_height);
            }
            events.push_back(event);
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const Key key = internal::key_from_scancode(e.key.scancode);
            if (key != Key::Unknown) {
                Input::Writer::key(input, key, e.key.down, e.key.repeat);
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            MouseButton button;
            if (mouse_button_from_sdl(e.button.button, button)) {
                Input::Writer::mouse_button(input, button, e.button.down);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            Input::Writer::mouse_move(input, e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            Input::Writer::wheel(input, e.wheel.x, e.wheel.y);
            break;
        default:
            break;
        }
    }
}

} // namespace tynima::platform
