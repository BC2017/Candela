#include "candela/core/Jobs.h"

#include "candela/core/Log.h"

#include <tracy/Tracy.hpp>

#include <chrono>

namespace candela {

void JobSystem::init(uint32_t threadCount) {
    CD_ASSERT(!s_running, "JobSystem already initialized");
    if (threadCount == 0) {
        const uint32_t hardware = std::thread::hardware_concurrency();
        threadCount = hardware > 1 ? hardware - 1 : 1;
    }
    s_running = true;
    s_workers.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i) {
        s_workers.emplace_back(&JobSystem::workerLoop);
    }
    CD_INFO("JobSystem: {} worker threads", threadCount);
}

void JobSystem::shutdown() {
    {
        std::scoped_lock lock(s_mutex);
        s_running = false;
    }
    s_wake.notify_all();
    for (std::thread& worker : s_workers) {
        worker.join();
    }
    s_workers.clear();
    s_queue.clear();
}

bool JobSystem::initialized() {
    return s_running;
}

void JobSystem::submit(std::function<void()> job, Counter* counter) {
    CD_ASSERT(s_running, "JobSystem::submit before init");
    if (counter != nullptr) {
        counter->pending.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock(s_mutex);
        s_queue.push_back({std::move(job), counter});
    }
    s_wake.notify_one();
}

void JobSystem::wait(Counter& counter) {
    ZoneScoped;
    // Help execute queued work while waiting instead of just blocking.
    while (counter.pending.load(std::memory_order_acquire) > 0) {
        Task task;
        {
            std::unique_lock lock(s_mutex);
            if (s_queue.empty()) {
                s_done.wait_for(lock, std::chrono::milliseconds(1));
                continue;
            }
            task = std::move(s_queue.front());
            s_queue.pop_front();
        }
        task.fn();
        if (task.counter != nullptr) {
            task.counter->pending.fetch_sub(1, std::memory_order_release);
            s_done.notify_all();
        }
    }
}

void JobSystem::workerLoop() {
    tracy::SetThreadName("candela-worker");
    while (true) {
        Task task;
        {
            std::unique_lock lock(s_mutex);
            s_wake.wait(lock, [] { return !s_queue.empty() || !s_running; });
            if (!s_running && s_queue.empty()) {
                return;
            }
            task = std::move(s_queue.front());
            s_queue.pop_front();
        }
        task.fn();
        if (task.counter != nullptr) {
            task.counter->pending.fetch_sub(1, std::memory_order_release);
            s_done.notify_all();
        }
    }
}

} // namespace candela
