#include <doctest/doctest.h>
#include <tynima/core/version.h>

#include <string>

TEST_CASE("version string agrees with the version components") {
    using namespace tynima::core;
    const std::string expected = std::to_string(kVersion.major) + "." + std::to_string(kVersion.minor) +
                                 "." + std::to_string(kVersion.patch);
    CHECK(version_string() == expected);
}

TEST_CASE("version string is stable across calls") {
    using namespace tynima::core;
    CHECK(version_string() == version_string()); // same static storage, not a fresh buffer
}
