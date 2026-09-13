// Private to platform/. The only place SDL headers are included is platform/
// and rhi/; tools/check_layering.py enforces that.
#pragma once

#include <SDL3/SDL.h>
#include <tynima/platform/input.h>

namespace tynima::platform::internal {

Key key_from_scancode(SDL_Scancode scancode) noexcept;

} // namespace tynima::platform::internal
