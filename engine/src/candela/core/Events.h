#pragma once

#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace candela {

// Minimal typed publish/subscribe bus with immediate (synchronous) dispatch.
// Subscribe returns nothing — subscriptions live as long as the bus; scoped
// unsubscription arrives when a real consumer needs it.
//
//   bus.subscribe<AssetReloadedEvent>([](const AssetReloadedEvent& e) {...});
//   bus.publish(AssetReloadedEvent{guid});
class EventBus {
public:
    template <typename Event>
    void subscribe(std::function<void(const Event&)> handler) {
        auto& list = m_handlers[std::type_index(typeid(Event))];
        list.push_back([handler = std::move(handler)](const void* event) {
            handler(*static_cast<const Event*>(event));
        });
    }

    template <typename Event>
    void publish(const Event& event) const {
        auto it = m_handlers.find(std::type_index(typeid(Event)));
        if (it == m_handlers.end()) {
            return;
        }
        for (const auto& handler : it->second) {
            handler(&event);
        }
    }

private:
    std::unordered_map<std::type_index,
                       std::vector<std::function<void(const void*)>>>
        m_handlers;
};

} // namespace candela
