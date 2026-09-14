#include <tynima/core/jobs.h>

#include <tynima/core/assert.h>
#include <tynima/core/profile.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace tynima::core {

namespace {

thread_local std::uint32_t t_thread_index = 0;

struct QueuedJob {
    Job job;
    JobCounter* counter;
};

// A bounded deque owned by one thread. The owner works at the back; thieves
// take from the front, so the two ends contend as little as possible. A
// mutex keeps it simple; Tracy will say if it ever needs to be lock-free.
class Deque {
public:
    static constexpr std::uint32_t kCapacity = 1024;

    bool push_back(const QueuedJob& job) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == kCapacity) {
            return false;
        }
        items_[(head_ + size_) % kCapacity] = job;
        ++size_;
        return true;
    }
    bool pop_back(QueuedJob& out) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) {
            return false;
        }
        --size_;
        out = items_[(head_ + size_) % kCapacity];
        return true;
    }
    bool steal_front(QueuedJob& out) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) {
            return false;
        }
        out = items_[head_];
        head_ = (head_ + 1) % kCapacity;
        --size_;
        return true;
    }

private:
    std::mutex mutex_;
    QueuedJob items_[kCapacity];
    std::uint32_t head_ = 0;
    std::uint32_t size_ = 0;
};

void name_current_thread(const char* name) noexcept {
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(_WIN32)
    wchar_t wide[64];
    int i = 0;
    for (; name[i] != '\0' && i < 63; ++i) {
        wide[i] = static_cast<wchar_t>(name[i]);
    }
    wide[i] = L'\0';
    SetThreadDescription(GetCurrentThread(), wide);
#else
    (void)name;
#endif
}

void prefer_performance_cores() noexcept {
#if defined(__APPLE__)
    // Not pinned: the scheduler keeps the right to move us, but it schedules
    // user-interactive work on the performance cores first.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

} // namespace

struct JobSystem::Impl {
    std::vector<Deque> queues; // index 0 is the creating thread, 1..n the workers
    std::vector<std::thread> workers;
    std::atomic<bool> stop{false};
    std::atomic<std::uint32_t> queued{0}; // jobs sitting in any deque
    std::mutex sleep_mutex;
    std::condition_variable sleep_cv;

    explicit Impl(std::uint32_t worker_count) : queues(worker_count + 1) {}

    void execute(QueuedJob& queued_job) noexcept {
        queued_job.job.function(queued_job.job.data);
        if (queued_job.counter != nullptr) {
            queued_job.counter->pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // One job from our own deque, or stolen from another. False when there is none.
    bool try_run_one(std::uint32_t self) noexcept {
        QueuedJob job{};
        bool found = queues[self].pop_back(job);
        for (std::uint32_t i = 1; !found && i < queues.size(); ++i) {
            found = queues[(self + i) % queues.size()].steal_front(job);
        }
        if (!found) {
            return false;
        }
        queued.fetch_sub(1, std::memory_order_acq_rel);
        execute(job);
        return true;
    }

    void worker_main(std::uint32_t index) noexcept {
        t_thread_index = index;
        char name[32];
        std::snprintf(name, sizeof(name), "tynima worker %u", index);
        name_current_thread(name);
        TY_PROFILE_THREAD(name);
        prefer_performance_cores();

        while (!stop.load(std::memory_order_acquire)) {
            if (try_run_one(index)) {
                continue;
            }
            // Nothing anywhere: spin briefly (work usually arrives in bursts),
            // then sleep until a submission wakes us.
            bool ran = false;
            for (int spin = 0; spin < 64 && !ran; ++spin) {
                std::this_thread::yield();
                ran = try_run_one(index);
            }
            if (!ran) {
                std::unique_lock<std::mutex> lock(sleep_mutex);
                sleep_cv.wait(lock, [&] {
                    return stop.load(std::memory_order_acquire) || queued.load(std::memory_order_acquire) > 0;
                });
            }
        }
    }
};

JobSystem::JobSystem() : JobSystem(Desc{}) {}

JobSystem::JobSystem(const Desc& desc) {
    std::uint32_t workers = desc.worker_count;
    if (workers == kAutoWorkers) {
        const std::uint32_t cores = performance_core_count();
        workers = cores > 1 ? cores - 1 : 0;
    }
    if (workers > 64) {
        workers = 64;
    }
    impl_ = std::make_unique<Impl>(workers);
    t_thread_index = 0;
    for (std::uint32_t i = 1; i <= workers; ++i) {
        impl_->workers.emplace_back([impl = impl_.get(), i] { impl->worker_main(i); });
    }
}

JobSystem::~JobSystem() {
    // Drain: anything still queued runs here, then the workers are released.
    while (impl_->try_run_one(0)) {
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->sleep_mutex);
        impl_->stop.store(true, std::memory_order_release);
    }
    impl_->sleep_cv.notify_all();
    for (std::thread& worker : impl_->workers) {
        worker.join();
    }
}

void JobSystem::run(Job job, JobCounter* counter) noexcept {
    TY_ASSERT(job.function != nullptr, "JobSystem::run: a job needs a function");
    if (counter != nullptr) {
        counter->pending_.fetch_add(1, std::memory_order_acq_rel);
    }
    const std::uint32_t self = t_thread_index < impl_->queues.size() ? t_thread_index : 0;
    QueuedJob queued_job{job, counter};
    if (!impl_->queues[self].push_back(queued_job)) {
        impl_->execute(queued_job); // full deque: do it now rather than wait
        return;
    }
    impl_->queued.fetch_add(1, std::memory_order_acq_rel);
    impl_->sleep_cv.notify_one();
}

void JobSystem::wait(JobCounter& counter) noexcept {
    TY_PROFILE_SCOPE_NAMED("jobs::wait");
    const std::uint32_t self = t_thread_index < impl_->queues.size() ? t_thread_index : 0;
    while (!counter.done()) {
        if (!impl_->try_run_one(self)) {
            std::this_thread::yield(); // our jobs are running elsewhere; let them finish
        }
    }
}

std::uint32_t JobSystem::worker_count() const noexcept {
    return static_cast<std::uint32_t>(impl_->workers.size());
}

std::uint32_t JobSystem::current_thread_index() noexcept {
    return t_thread_index;
}

std::uint32_t JobSystem::performance_core_count() noexcept {
#if defined(__APPLE__)
    // Apple Silicon reports its performance cluster as perflevel0.
    int count = 0;
    std::size_t size = sizeof(count);
    if (sysctlbyname("hw.perflevel0.logicalcpu", &count, &size, nullptr, 0) == 0 && count > 0) {
        return static_cast<std::uint32_t>(count);
    }
#endif
    const unsigned all = std::thread::hardware_concurrency();
    return all > 0 ? all : 1;
}

} // namespace tynima::core
