#pragma once

#include <deque>
#include <functional>

namespace candela {

// Collects destruction lambdas and runs them in reverse order. Used for
// teardown paths where Vulkan object lifetimes must unwind in creation order.
class DeletionQueue {
public:
    void push(std::function<void()> fn) { m_queue.push_back(std::move(fn)); }

    void flush() {
        for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it) {
            (*it)();
        }
        m_queue.clear();
    }

private:
    std::deque<std::function<void()>> m_queue;
};

} // namespace candela
