// The editor sees the engine only through tynima.h — the same surface a game
// gets. If something the editor needs isn't in the SDK, the SDK is incomplete.
#include <tynima.h>

#include <cstdio>

int main() {
    std::printf("tynima editor - engine %s\n", tynima_get_version_string());
    return 0;
}
