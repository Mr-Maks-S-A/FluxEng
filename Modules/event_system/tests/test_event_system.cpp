#include <events/event_dispatcher.hpp>
#include <events/event_queue.hpp>
#include <print>
#include <source_location>
#include <cstdlib>
#include <vector>

namespace test_runner {
    inline size_t passed_count = 0;
    inline size_t failed_count = 0;

    void assert_true(bool condition, std::string_view msg, std::source_location loc = std::source_location::current()) {
        if (condition) {
            ++passed_count;
        } else {
            ++failed_count;
            std::println(stderr, "  [FAIL] {} ({}:{})", msg, loc.file_name(), loc.line());
        }
    }
}

#define TEST_ASSERT(cond) test_runner::assert_true((cond), #cond)

using namespace engine::events;

void test_push_and_clear() {
    std::println("[RUN] test_push_and_clear...");
    ThreadLocalEventQueue tq;
    tq.push(1, EventRank::Rank0_Input, 100, 0);
    tq.push(2, EventRank::Rank2_Physics, 100, 1);

    TEST_ASSERT(tq.get_batch().size() == 2);
    tq.get_batch().clear();
    TEST_ASSERT(tq.get_batch().size() == 0);
}

void test_merge_and_sort_ordering() {
    std::println("[RUN] test_merge_and_sort_ordering...");
    ThreadLocalEventQueue q1;
    ThreadLocalEventQueue q2;

    // Пушим события вразнобой
    q1.push(10, EventRank::Rank3_VoxelLogic, 1, 0);
    q1.push(20, EventRank::Rank0_Input, 1, 1);

    q2.push(30, EventRank::Rank2_Physics, 1, 2);
    q2.push(40, EventRank::Rank1_Validation, 1, 3);

    EventDispatcher dispatcher;
    std::vector<ThreadLocalEventQueue> queues;
    queues.push_back(std::move(q1));
    queues.push_back(std::move(q2));

    dispatcher.merge_and_sort(queues);

    const auto& batch = dispatcher.get_main_batch();
    const auto indices = dispatcher.get_sorted_indices();

    TEST_ASSERT(batch.size() == 4);

    // Ожидаемый порядок по рангам (0 -> 1 -> 2 -> 3)
    TEST_ASSERT(batch.event_ids[indices[0]] == 20); // Rank0
    TEST_ASSERT(batch.event_ids[indices[1]] == 40); // Rank1
    TEST_ASSERT(batch.event_ids[indices[2]] == 30); // Rank2
    TEST_ASSERT(batch.event_ids[indices[3]] == 10); // Rank3
}

void test_tick_priority() {
    std::println("[RUN] test_tick_priority...");
    ThreadLocalEventQueue q;
    
    // Событие ранга 5 на тике 1 должно быть раньше события ранга 0 на тике 2
    q.push(100, EventRank::Rank5_RenderExport, 1, 0);
    q.push(200, EventRank::Rank0_Input, 2, 1);

    EventDispatcher dispatcher;
    std::vector<ThreadLocalEventQueue> queues;
    queues.push_back(std::move(q));

    dispatcher.merge_and_sort(queues);

    const auto& batch = dispatcher.get_main_batch();
    const auto indices = dispatcher.get_sorted_indices();

    TEST_ASSERT(batch.event_ids[indices[0]] == 100);
    TEST_ASSERT(batch.event_ids[indices[1]] == 200);
}

int main() {
    std::println("=== RUNNING EVENT_SYSTEM UNIT TESTS ===");
    
    test_push_and_clear();
    test_merge_and_sort_ordering();
    test_tick_priority();

    std::println("\nResults: {} PASSED, {} FAILED", test_runner::passed_count, test_runner::failed_count);

    return test_runner::failed_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
