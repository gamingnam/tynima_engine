#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace tynima::core {

// One unit of work: a function and the data it runs on. Nothing is copied or
// owned, so the data must live until the job has run — which is guaranteed
// when the code that submitted it waits on a JobCounter before returning.
struct Job {
    void (*function)(void* data) = nullptr;
    void* data = nullptr;
};

// Counts jobs still running; wait() on it to join them.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
    [[nodiscard]] bool done() const noexcept { return pending_.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;
    std::atomic<std::uint32_t> pending_{0};
};

// Work-stealing job system. Every participating thread — the workers and
// whoever calls wait() — owns a deque: it pushes and pops at the back, and
// takes from the front of the others when its own runs dry. wait() never
// blocks idle; it runs jobs until the counter reaches zero, so a job may
// itself submit and wait on more jobs without deadlocking the pool.
class JobSystem {
public:
    static constexpr std::uint32_t kAutoWorkers = 0xFFFFFFFFu;

    struct Desc {
        // kAutoWorkers = one fewer than the performance cores (the calling
        // thread is the last one). 0 is allowed: everything runs on the caller.
        std::uint32_t worker_count = kAutoWorkers;
    };

    JobSystem();
    explicit JobSystem(const Desc& desc);
    ~JobSystem(); // finishes what is queued, then joins the workers
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Queues a job on the calling thread's deque. Never blocks: a full deque
    // runs the job inline instead.
    void run(Job job, JobCounter* counter = nullptr) noexcept;

    // Runs jobs until `counter` reaches zero.
    void wait(JobCounter& counter) noexcept;

    // Runs fn(begin, end) over [0, count) in chunks of about `chunk_size`, on
    // every thread available, and returns when all of it is done. fn is
    // called concurrently, so it must not touch shared state unsynchronized.
    template <typename Fn>
    void parallel_for(std::uint32_t count, std::uint32_t chunk_size, Fn&& fn) noexcept {
        if (count == 0) {
            return;
        }
        using Callable = std::remove_reference_t<Fn>; // fn may arrive as an lvalue
        struct Task {
            const Callable* fn;
            std::uint32_t begin;
            std::uint32_t end;
        };
        // At most kMaxChunks chunks so the tasks fit on this stack; more
        // granularity than that buys nothing with a handful of threads.
        if (chunk_size == 0) {
            chunk_size = 1;
        }
        const std::uint32_t min_chunk = (count + kMaxChunks - 1) / kMaxChunks;
        if (chunk_size < min_chunk) {
            chunk_size = min_chunk;
        }
        Task tasks[kMaxChunks];
        JobCounter counter;
        std::uint32_t task_count = 0;
        for (std::uint32_t begin = 0; begin < count; begin += chunk_size) {
            const std::uint32_t end = begin + chunk_size < count ? begin + chunk_size : count;
            tasks[task_count] = {&fn, begin, end};
            run({[](void* data) {
                     const Task* task = static_cast<Task*>(data);
                     (*task->fn)(task->begin, task->end);
                 },
                 &tasks[task_count]},
                &counter);
            ++task_count;
        }
        wait(counter);
    }

    [[nodiscard]] std::uint32_t worker_count() const noexcept;
    // 0 on the thread that created the system (and any other non-worker
    // thread); 1..worker_count() on the workers.
    [[nodiscard]] static std::uint32_t current_thread_index() noexcept;

    // Performance cores on this machine (all cores where the OS does not say).
    [[nodiscard]] static std::uint32_t performance_core_count() noexcept;

private:
    static constexpr std::uint32_t kMaxChunks = 256;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tynima::core
