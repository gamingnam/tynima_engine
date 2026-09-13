#include <tynima/core/profile.h>

namespace tynima::core {

bool profiler_connected() noexcept {
#if defined(TYNIMA_PROFILE)
    return TracyIsConnected;
#else
    return false;
#endif
}

} // namespace tynima::core
