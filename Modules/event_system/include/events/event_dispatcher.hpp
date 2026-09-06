#pragma once
#include "event_queue.hpp"
#include <vector>
#include <numeric>

namespace engine::events {

class EventDispatcher {
public:
    // Фаза Merge & Sort: Сливает все Thread-Local очереди в одну и сортирует по (Tick, Rank)
    void merge_and_sort(std::span<ThreadLocalEventQueue> thread_queues) {
        m_main_batch.clear();
        
        // 1. Оцениваем суммарный размер и резервируем память O(1)
        size_t total_events = 0;
        for (auto& tq : thread_queues) {
            total_events += tq.get_batch().size();
        }
        m_main_batch.reserve(total_events);

        // 2. Быстрое слияние (memcpy style)
        for (auto& tq : thread_queues) {
            auto& b = tq.get_batch();
            m_main_batch.event_ids.insert(m_main_batch.event_ids.end(), b.event_ids.begin(), b.event_ids.end());
            m_main_batch.ranks.insert(m_main_batch.ranks.end(), b.ranks.begin(), b.ranks.end());
            m_main_batch.ticks.insert(m_main_batch.ticks.end(), b.ticks.begin(), b.ticks.end());
            m_main_batch.payload_indices.insert(m_main_batch.payload_indices.end(), b.payload_indices.begin(), b.payload_indices.end());
            
            b.clear(); // Очищаем локальную очередь потока
        }

        // 3. Индексная сортировка для сохранения SoA формата и гарантированного ДЕТЕРМИНИЗМА
        m_permutation.resize(m_main_batch.size());
        std::iota(m_permutation.begin(), m_permutation.end(), 0);

        std::stable_sort(m_permutation.begin(), m_permutation.end(), [this](size_t a, size_t b) {
            if (m_main_batch.ticks[a] != m_main_batch.ticks[b]) {
                return m_main_batch.ticks[a] < m_main_batch.ticks[b];
            }
            return static_cast<uint8_t>(m_main_batch.ranks[a]) < static_cast<uint8_t>(m_main_batch.ranks[b]);
        });
    }

    const EventBatchSoA& get_main_batch() const noexcept { return m_main_batch; }
    std::span<const size_t> get_sorted_indices() const noexcept { return m_permutation; }

private:
    EventBatchSoA m_main_batch;
    std::vector<size_t> m_permutation;
};

} // namespace engine::events
