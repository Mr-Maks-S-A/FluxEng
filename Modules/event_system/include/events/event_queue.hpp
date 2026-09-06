#pragma once
#include <vector>
#include <cstdint>
#include <span>
#include <algorithm>

namespace engine::events {

enum class EventRank : uint8_t {
    Rank0_Input = 0,
    Rank1_Validation,
    Rank2_Physics,
    Rank3_VoxelLogic,
    Rank4_Presentation,
    Rank5_RenderExport,
    Count
};

// SoA Пачка событий
struct EventBatchSoA {
    std::vector<uint32_t> event_ids;
    std::vector<EventRank> ranks;
    std::vector<uint64_t> ticks;
    std::vector<uint32_t> payload_indices; // Индексы в сыром пуле данных

    void reserve(size_t capacity) {
        event_ids.reserve(capacity);
        ranks.reserve(capacity);
        ticks.reserve(capacity);
        payload_indices.reserve(capacity);
    }

    void clear() noexcept {
        event_ids.clear();
        ranks.clear();
        ticks.clear();
        payload_indices.clear();
    }

    size_t size() const noexcept { return event_ids.size(); }
};

// Локальная очередь для одного потока
class ThreadLocalEventQueue {
public:
    void push(uint32_t event_id, EventRank rank, uint64_t tick, uint32_t payload_idx) {
        m_batch.event_ids.push_back(event_id);
        m_batch.ranks.push_back(rank);
        m_batch.ticks.push_back(tick);
        m_batch.payload_indices.push_back(payload_idx);
    }

    EventBatchSoA& get_batch() noexcept { return m_batch; }

private:
    EventBatchSoA m_batch;
};

} // namespace engine::events
