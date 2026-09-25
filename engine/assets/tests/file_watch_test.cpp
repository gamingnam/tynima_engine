#include <tynima/assets/file_watch.h>
#include <tynima/platform/file.h>
#include <tynima/platform/time.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace tynima;

namespace {

// Where this machine puts temporary files. std::filesystem asks the OS —
// TMPDIR where it is set, the user's Temp on Windows — rather than assuming
// "/tmp", which on Windows names a directory on whatever drive the test
// happens to run from and usually is not there at all.
std::string temp_path(const char* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = "/tmp";
    }
    return (dir / name).lexically_normal().generic_string();
}

// Writes until the file's recorded time moves: a write within the clock's
// resolution of the last would otherwise look like no change at all.
void touch(const std::string& path, const char* text) {
    const std::uint64_t before = platform::file_write_time(path.c_str());
    for (int attempt = 0; attempt < 1000; ++attempt) {
        REQUIRE(platform::write_file(path.c_str(), text, std::char_traits<char>::length(text)));
        if (platform::file_write_time(path.c_str()) != before) {
            return;
        }
        platform::sleep_ns(1'000'000);
    }
    FAIL("the file's write time never moved");
}

} // namespace

TEST_CASE("a file watch reports a change once it has settled, and nothing else") {
    const std::string path = temp_path("tynima_watch_test.txt");
    touch(path, "one");
    assets::FileWatch watch(1.0);
    const assets::FileWatch::Id id = watch.watch(path.c_str());
    CHECK(watch.path(id) == path);
    CHECK(watch.count() == 1);
    assets::FileWatch::Id changed[4];

    // Nothing has changed: nothing, however often it is asked.
    CHECK(watch.poll(0.0, changed, 4) == 0);
    CHECK(watch.poll(5.0, changed, 4) == 0);

    // A change is seen at the next interval, and reported one interval later.
    touch(path, "two");
    CHECK(watch.poll(5.5, changed, 4) == 0); // too soon after the last poll: not even looked at
    CHECK(watch.poll(6.0, changed, 4) == 0); // seen, still settling
    CHECK(watch.poll(6.5, changed, 4) == 0); // between intervals
    REQUIRE(watch.poll(7.0, changed, 4) == 1);
    CHECK(changed[0] == id);
    CHECK(watch.poll(8.0, changed, 4) == 0); // reported once

    // A file that keeps changing is not reported until it stops.
    touch(path, "three");
    CHECK(watch.poll(9.0, changed, 4) == 0);
    touch(path, "four");
    CHECK(watch.poll(10.0, changed, 4) == 0); // moved again: the clock restarts
    CHECK(watch.poll(11.0, changed, 4) == 1);

    // Gone is not a change; coming back is — even wearing the write time it
    // left with, which is what Windows hands a file when the delete and the
    // write land in one tick of its clock, some 15 ms wide. The time is put
    // back by hand here so that every platform tests the same thing.
    std::error_code ec;
    const std::filesystem::file_time_type was = std::filesystem::last_write_time(path, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(platform::remove_file(path.c_str()));
    CHECK(watch.poll(12.0, changed, 4) == 0);
    CHECK(watch.poll(13.0, changed, 4) == 0);
    touch(path, "five");
    std::filesystem::last_write_time(path, was, ec); // as if no time had passed
    REQUIRE_FALSE(ec);
    REQUIRE(platform::file_write_time(path.c_str()) != 0);
    CHECK(watch.poll(14.0, changed, 4) == 0);
    CHECK(watch.poll(15.0, changed, 4) == 1);

    // No room to report: kept for a poll with room.
    touch(path, "six");
    CHECK(watch.poll(16.0, changed, 4) == 0);
    CHECK(watch.poll(17.0, changed, 0) == 0);
    CHECK(watch.poll(18.0, changed, 4) == 1);

    // Acknowledged: what the owner wrote itself is not news.
    touch(path, "six and a half");
    watch.acknowledge(id);
    CHECK(watch.poll(18.5, changed, 4) == 0);
    CHECK(watch.poll(19.5, changed, 4) == 0);

    watch.unwatch(id);
    CHECK(watch.path(id).empty());
    touch(path, "seven");
    CHECK(watch.poll(20.5, changed, 4) == 0);
    CHECK(watch.poll(21.5, changed, 4) == 0);
    // The slot is reused.
    CHECK(watch.watch(path.c_str()) == id);
    CHECK(watch.watch("/no/such/file") == 1);
    CHECK(watch.count() == 2);
    (void)platform::remove_file(path.c_str());
}
