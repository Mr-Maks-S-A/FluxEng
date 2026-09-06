#include <events/event_dispatcher.hpp>
#include <events/event_queue.hpp>
#include <chrono>
#include <print>
#include <random>
#include <vector>

using namespace engine::events;

void run_benchmark(size_t num_threads, size_t events_per_thread, size_t iterations) {
    const size_t total_events = num_threads * events_per_thread;
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint8_t> rank_dist(0, static_cast<uint8_t>(EventRank::Count) - 1);

    std::println("--------------------------------------------------");
    std::println("Benchmarking: {} Threads, {} Events/Thread (Total: {})", 
                 num_threads, events_per_thread, total_events);

    double total_time_ms = 0.0;

    for (size_t iter = 0; iter < iterations; ++iter) {
        // Подготовка данных
        std::vector<ThreadLocalEventQueue> queues(num_threads);
        for (size_t t = 0; t < num_threads; ++t) {
            queues[t].get_batch().reserve(events_per_thread);
            for (size_t i = 0; i < events_per_thread; ++i) {
                queues[t].push(
                    static_cast<uint32_t>(i),
                    static_cast<EventRank>(rank_dist(rng)),
                    1, // tick
                    static_cast<uint32_t>(i)
                );
            }
        }

        EventDispatcher dispatcher;

        // Замер только Merge & Sort
        auto start = std::chrono::high_resolution_clock::now();
        
        dispatcher.merge_and_sort(queues);
        
        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> duration = end - start;
        total_time_ms += duration.count();
    }

    double avg_time_ms = total_time_ms / static_cast<double>(iterations);
    double m_events_per_sec = (static_cast<double>(total_events) / 1'000'000.0) / (avg_time_ms / 1000.0);

    std::println("Avg Time: {:.3f} ms | Throughput: {:.2f} Million events/sec", 
                 avg_time_ms, m_events_per_sec);
}

int main() {
    std::println("=== EVENT SYSTEM BENCHMARK (NO DEPENDENCIES) ===");
    
    const size_t iterations = 10;

    // 100,000 событий
    run_benchmark(4, 25'000, iterations);

    // 1,000,000 событий на 8 потоков
    run_benchmark(8, 125'000, iterations);

    // 1,000,000 событий на 16 потоков
    run_benchmark(16, 62'500, iterations);

    return 0;
}
