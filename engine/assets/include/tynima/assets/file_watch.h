#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Files watched for change, by asking. Every `interval` seconds the watch
// stats each file it holds; one whose write time has moved, and then stayed
// put for a whole further interval, is reported once. The settling matters:
// an exporter or the cooker writes a file over some milliseconds, and a
// reader that pounced on the first change would read half of one. A file
// that is missing for a while is not a change; it is reported when it is
// back and different.
//
// Asking rather than being told — no FSEvents, no inotify, no
// ReadDirectoryChangesW — because a few hundred stats a quarter of a second
// cost nothing against a frame, the behaviour is the same on every
// platform, and there is no thread to synchronise with. It is what the game
// module's reload has always done.
namespace tynima::assets {

class FileWatch {
public:
    using Id = std::uint32_t;
    static constexpr Id kNone = 0xFFFFFFFFu;

    explicit FileWatch(double interval_seconds = 0.25) noexcept : interval_(interval_seconds) {}

    // Starts watching; the file's write time now is the baseline, so only a
    // later change is reported. The one place the watch allocates.
    Id watch(const char* path);
    void unwatch(Id id) noexcept;
    // Takes the file as it is now for seen and reported: what the owner
    // itself just wrote is not reported back to it.
    void acknowledge(Id id) noexcept;
    [[nodiscard]] const std::string& path(Id id) const noexcept;
    [[nodiscard]] std::uint32_t count() const noexcept { return static_cast<std::uint32_t>(entries_.size()); }

    // Stats every file if an interval has passed since the last time, and
    // writes the ids of those that changed and settled into `changed`, up
    // to `capacity`. Returns how many. Never allocates: safe every frame.
    std::uint32_t poll(double now, Id* changed, std::uint32_t capacity) noexcept;

private:
    struct Entry {
        std::string path;
        std::uint64_t reported = 0; // the write time the owner last saw
        std::uint64_t seen = 0;     // the write time of the last poll
        double seen_since = 0.0;    // when `seen` first appeared
        bool live = false;
    };
    std::vector<Entry> entries_;
    double interval_;
    double last_poll_ = -1.0e300;
};

} // namespace tynima::assets
