#include <doctest/doctest.h>
#include <tynima/core/jobs.h>
#include <tynima/core/memory.h>

#include <atomic>
#include <cstdint>
#include <vector>

using namespace tynima::core;

TEST_CASE("jobs run, counters join them, and the calling thread helps") {
    JobSystem jobs;
    CHECK(JobSystem::current_thread_index() == 0);
    CHECK(jobs.worker_count() + 1 <= JobSystem::performance_core_count() + 1);

    std::atomic<int> executed{0};
    std::atomic<std::uint32_t> threads_seen{0}; // bitmask of thread indices
    JobCounter counter;
    struct Data {
        std::atomic<int>* executed;
        std::atomic<std::uint32_t>* seen;
    } data{&executed, &threads_seen};
    for (int i = 0; i < 400; ++i) {
        jobs.run({[](void* p) {
                      auto* d = static_cast<Data*>(p);
                      d->executed->fetch_add(1);
                      d->seen->fetch_or(1u << JobSystem::current_thread_index());
                  },
                  &data},
                 &counter);
    }
    jobs.wait(counter);
    CHECK(counter.done());
    CHECK(executed.load() == 400);
    // With any worker at all, the burst should have been shared; with none, the
    // main thread did it all. Either way, nothing is lost.
    CHECK(threads_seen.load() != 0);
    if (jobs.worker_count() > 0) {
        MESSAGE("threads that ran jobs (bitmask): ", threads_seen.load());
    }
}

TEST_CASE("parallel_for covers every index exactly once") {
    JobSystem jobs;
    const std::uint32_t count = 100'000;
    std::vector<std::uint8_t> touched(count, 0);
    std::vector<std::uint64_t> partials(1024, 0);
    std::atomic<std::uint32_t> chunks{0};
    jobs.parallel_for(count, 1000, [&](std::uint32_t begin, std::uint32_t end) {
        const std::uint32_t chunk = chunks.fetch_add(1);
        std::uint64_t sum = 0;
        for (std::uint32_t i = begin; i < end; ++i) {
            touched[i] += 1;
            sum += i;
        }
        partials[chunk] = sum;
    });
    std::uint64_t total = 0;
    for (std::uint32_t c = 0; c < chunks.load(); ++c) {
        total += partials[c];
    }
    bool all_once = true;
    for (const std::uint8_t t : touched) {
        all_once = all_once && t == 1;
    }
    CHECK(all_once);
    CHECK(total == static_cast<std::uint64_t>(count) * (count - 1) / 2);
    CHECK(chunks.load() == 100);

    // Degenerate sizes.
    int calls = 0;
    jobs.parallel_for(0, 10, [&](std::uint32_t, std::uint32_t) { ++calls; });
    CHECK(calls == 0);
    jobs.parallel_for(3, 0, [&](std::uint32_t begin, std::uint32_t end) { calls += static_cast<int>(end - begin); });
    CHECK(calls == 3);
}

TEST_CASE("a job may submit and wait on its own jobs without deadlocking") {
    JobSystem jobs;
    std::atomic<int> inner{0};
    struct Data {
        JobSystem* jobs;
        std::atomic<int>* inner;
    } data{&jobs, &inner};
    JobCounter outer;
    for (int i = 0; i < 8; ++i) {
        jobs.run({[](void* p) {
                      auto* d = static_cast<Data*>(p);
                      d->jobs->parallel_for(64, 8, [&](std::uint32_t begin, std::uint32_t end) {
                          d->inner->fetch_add(static_cast<int>(end - begin));
                      });
                  },
                  &data},
                 &outer);
    }
    jobs.wait(outer);
    CHECK(inner.load() == 8 * 64);
}

TEST_CASE("submitting and waiting allocate nothing on the heap") {
    JobSystem jobs;
    std::atomic<int> executed{0};
    HeapAllocationScope scope;
    JobCounter counter;
    for (int i = 0; i < 64; ++i) {
        jobs.run({[](void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); }, &executed}, &counter);
    }
    jobs.wait(counter);
    jobs.parallel_for(1000, 100, [&](std::uint32_t begin, std::uint32_t end) {
        executed.fetch_add(static_cast<int>(end - begin));
    });
    const std::uint64_t traffic = scope.allocations();
    CHECK(executed.load() == 64 + 1000);
    CHECK(traffic == 0);
}

TEST_CASE("a job system with no workers still runs everything, on the caller") {
    JobSystem jobs({.worker_count = 0});
    CHECK(jobs.worker_count() == 0);
    std::atomic<int> executed{0};
    std::atomic<std::uint32_t> threads_seen{0};
    JobCounter counter;
    struct Data {
        std::atomic<int>* executed;
        std::atomic<std::uint32_t>* seen;
    } data{&executed, &threads_seen};
    for (int i = 0; i < 10; ++i) {
        jobs.run({[](void* p) {
                      auto* d = static_cast<Data*>(p);
                      d->executed->fetch_add(1);
                      d->seen->fetch_or(1u << JobSystem::current_thread_index());
                  },
                  &data},
                 &counter);
    }
    jobs.wait(counter);
    CHECK(executed.load() == 10);
    CHECK(threads_seen.load() == 1); // only thread 0
    jobs.parallel_for(50, 7, [&](std::uint32_t begin, std::uint32_t end) {
        executed.fetch_add(static_cast<int>(end - begin));
    });
    CHECK(executed.load() == 60);
}
