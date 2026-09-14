#pragma once

// Shared between the sandbox (which creates entities with it) and the game
// module (which runs the system over it). A component is a name and plain
// data; the World checks that both sides agree on the layout.
struct Spin {
    static constexpr const char* kName = "Spin";
    float radians_per_second = 0.0f;
};
