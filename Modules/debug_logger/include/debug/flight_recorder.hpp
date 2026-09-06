#pragma once
#include <array>
#include <string_view>
#include <source_location>
#include <fstream>
#include <mutex>
#include <cstdint>

namespace engine::debug {

struct TraceEvent {
    uint64_t tick{0};
    uint32_t thread_id{0};
    std::string_view message{};
    std::source_location location{};
};

template<size_t Capacity = 1024>
class FlightRecorder {
public:
    void record(std::string_view msg, uint64_t tick = 0, std::source_location loc = std::source_location::current()) noexcept {
        size_t idx = m_head.fetch_add(1, std::memory_order_relaxed) % Capacity;
        m_buffer[idx] = TraceEvent{
            .tick = tick,
            .thread_id = 0, // Можно подставить get_thread_id()
            .message = msg,
            .location = loc
        };
    }

    void dump_to_file(std::string_view filepath) const {
        std::ofstream file(filepath.data(), std::ios::trunc);
        if (!file.is_open()) return;

        file << "=== FLIGHT RECORDER CRASH DUMP ===\n";
        size_t current_head = m_head.load(std::memory_order_relaxed);
        size_t start = current_head > Capacity ? current_head % Capacity : 0;
        size_t count = std::min(current_head, Capacity);

        for (size_t i = 0; i < count; ++i) {
            size_t idx = (start + i) % Capacity;
            const auto& e = m_buffer[idx];
            file << "[" << e.tick << "] "
                 << e.location.file_name() << ":" << e.location.line()
                 << " (" << e.location.function_name() << ") -> "
                 << e.message << "\n";
        }
    }

private:
    std::array<TraceEvent, Capacity> m_buffer{};
    std::atomic<size_t> m_head{0};
};

inline FlightRecorder<2048> g_flight_recorder;

} // namespace engine::debug
