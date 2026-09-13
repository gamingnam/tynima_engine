#include <tynima.h>

#include <tynima/core/version.h>

extern "C" {

tynima_version tynima_get_version(void) {
    return tynima_version{tynima::core::kVersion.major, tynima::core::kVersion.minor,
                          tynima::core::kVersion.patch};
}

const char* tynima_get_version_string(void) {
    return tynima::core::version_string();
}

} // extern "C"
