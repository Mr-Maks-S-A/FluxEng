#pragma once
#include <EventSystem/Core/DeterministicTypeRegistry.hpp>
#include <EventSystem/Storage/IEventBucket.hpp>
#include <unordered_map>
#include <memory>
#include <spdlog/spdlog.h>

namespace EventSystem::Bus {

class EventBus {
private:
    std::unordered_map<Core::TypeId, std::unique_ptr<Storage::IEventBucket>> buckets;

public:
    template<typename BucketType>
    void register_event(std::size_t reserve_capacity = 512) {
        constexpr Core::TypeId id = Core::DeterministicTypeRegistry::get_id<BucketType>();
        
        if (buckets.contains(id)) {
            spdlog::warn("Event bucket for '{}' already registered!", 
                         Core::DeterministicTypeRegistry::get_name<BucketType>());
            return;
        }

        auto bucket = std::make_unique<BucketType>();
        bucket->reserve(reserve_capacity);
        buckets[id] = std::move(bucket);
    }

    template<typename BucketType, typename... Args>
    void emit(Args&&... args) {
        constexpr Core::TypeId id = Core::DeterministicTypeRegistry::get_id<BucketType>();
        auto it = buckets.find(id);
        
        if (it != buckets.end()) {
            auto* bucket = static_cast<BucketType*>(it->second.get());
            bucket->push(std::forward<Args>(args)...);
        } else {
            spdlog::error("Event bucket for '{}' is not registered in EventBus!", 
                          Core::DeterministicTypeRegistry::get_name<BucketType>());
        }
    }

    template<typename BucketType>
    [[nodiscard]] BucketType* get_bucket() {
        constexpr Core::TypeId id = Core::DeterministicTypeRegistry::get_id<BucketType>();
        auto it = buckets.find(id);
        if (it != buckets.end()) {
            return static_cast<BucketType*>(it->second.get());
        }
        return nullptr;
    }

    void clear_all() {
        for (auto& [id, bucket] : buckets) {
            bucket->clear();
        }
    }
};

} // namespace EventSystem::Bus