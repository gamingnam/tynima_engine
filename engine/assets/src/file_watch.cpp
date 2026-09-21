#include <tynima/assets/file_watch.h>

#include <tynima/platform/file.h>

namespace tynima::assets {

FileWatch::Id FileWatch::watch(const char* path) {
    Entry entry;
    entry.path = path != nullptr ? path : "";
    entry.reported = entry.seen = platform::file_write_time(entry.path.c_str());
    entry.seen_since = last_poll_;
    entry.live = true;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].live) {
            entries_[i] = std::move(entry);
            return static_cast<Id>(i);
        }
    }
    entries_.push_back(std::move(entry));
    return static_cast<Id>(entries_.size() - 1);
}

void FileWatch::unwatch(Id id) noexcept {
    if (id < entries_.size()) {
        entries_[id].live = false;
    }
}

void FileWatch::acknowledge(Id id) noexcept {
    if (id < entries_.size() && entries_[id].live) {
        Entry& entry = entries_[id];
        entry.reported = entry.seen = platform::file_write_time(entry.path.c_str());
        entry.seen_since = last_poll_;
    }
}

const std::string& FileWatch::path(Id id) const noexcept {
    static const std::string none;
    return id < entries_.size() && entries_[id].live ? entries_[id].path : none;
}

std::uint32_t FileWatch::poll(double now, Id* changed, std::uint32_t capacity) noexcept {
    if (now - last_poll_ < interval_) {
        return 0;
    }
    last_poll_ = now;
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry& entry = entries_[i];
        if (!entry.live) {
            continue;
        }
        const std::uint64_t time = platform::file_write_time(entry.path.c_str());
        if (time == 0) {
            continue; // gone for the moment: an exporter between delete and write
        }
        if (time != entry.seen) {
            entry.seen = time;
            entry.seen_since = now;
            continue; // still moving: wait for it to settle
        }
        if (entry.seen != entry.reported && now - entry.seen_since >= interval_ && count < capacity) {
            entry.reported = entry.seen;
            changed[count++] = static_cast<Id>(i); // past capacity: kept for the next poll
        }
    }
    return count;
}

} // namespace tynima::assets
