#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace candela {

// Shared-queue thread pool. Counters group related jobs so callers can wait
// for completion. Work stealing arrives when profiling shows queue contention
// — at current job sizes (whole asset imports) it cannot.
class JobSystem {
public:
    // A counter tracks outstanding jobs submitted against it.
    struct Counter {
        std::atomic<uint32_t> pending{0};
    };

    static void init(uint32_t threadCount = 0); // 0 = hardware - 1
    static void shutdown();

    static void submit(std::function<void()> job, Counter* counter = nullptr);

    // Blocks until every job submitted against `counter` has finished.
    static void wait(Counter& counter);

    static bool initialized();

private:
    static void workerLoop();

    struct Task {
        std::function<void()> fn;
        Counter* counter = nullptr;
    };

    static inline std::vector<std::thread> s_workers;
    static inline std::deque<Task> s_queue;
    static inline std::mutex s_mutex;
    static inline std::condition_variable s_wake;
    static inline std::condition_variable s_done;
    static inline bool s_running = false;
};

} // namespace candela
