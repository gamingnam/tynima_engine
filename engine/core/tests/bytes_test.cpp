#include <tynima/core/bytes.h>

#include <doctest/doctest.h>

#include <cstring>
#include <utility>

using tynima::core::Bytes;

TEST_CASE("Bytes holds a block: sized, assigned, copied, moved and compared") {
    Bytes empty;
    CHECK(empty.empty());
    CHECK(empty.size() == 0);
    CHECK(empty.data() == nullptr);
    CHECK(empty == Bytes());

    Bytes sized(16);
    CHECK(sized.size() == 16);
    CHECK(sized.data() != nullptr);
    std::memset(sized.data(), 7, 16);
    CHECK(sized[15] == 7);

    Bytes from("hello", 5);
    CHECK(from.size() == 5);
    CHECK(std::memcmp(from.data(), "hello", 5) == 0);
    CHECK(from.end() - from.begin() == 5);
    CHECK_FALSE(from == sized);

    const Bytes copy = from;
    CHECK(copy == from);
    CHECK(copy.data() != from.data());
    Bytes moved = std::move(from);
    CHECK(moved == copy);
    CHECK(from.empty()); // NOLINT(bugprone-use-after-move): what a move leaves is defined here
    CHECK(from.data() == nullptr);

    // Growing keeps the prefix; shrinking keeps that much; clearing frees.
    moved.resize(8);
    CHECK(moved.size() == 8);
    CHECK(std::memcmp(moved.data(), "hello", 5) == 0);
    moved.resize(2);
    CHECK(moved == Bytes("he", 2));
    moved.assign("bytes", 5);
    CHECK(moved == Bytes("bytes", 5));
    moved = copy;
    CHECK(moved == Bytes("hello", 5));
    moved.clear();
    CHECK(moved.empty());
    CHECK(moved.data() == nullptr);
    moved.resize(0);
    CHECK(moved.empty());
    // Self-assignment is a no-op.
    Bytes& same = moved;
    moved = same;
    CHECK(moved.empty());
}
