/*
 * tynima.h — the Tynima public C ABI.
 *
 * Everything a game or the editor may call is declared here, in C, so it can
 * be bound from any language and survive hot reload across a dylib boundary.
 * Engine internals (tynima/<module>/...) are never visible through this header.
 *
 * Phase 0: version query only. Grows with the SDK in Phase 6.
 */
#ifndef TYNIMA_H
#define TYNIMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tynima_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} tynima_version;

tynima_version tynima_get_version(void);

/* "major.minor.patch", static storage. */
const char* tynima_get_version_string(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TYNIMA_H */
