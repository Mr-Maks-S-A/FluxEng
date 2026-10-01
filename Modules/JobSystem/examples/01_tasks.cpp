/**
 * @file 01_tasks.cpp
 * @brief Задачи и счётчики: run/wait, TaskGroup, вложенные задачи, исключение из задачи.
 */

#include <JobSystem/JobSystem.hpp>

#include <atomic>
#include <cstdio>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

int main() {
    JobSystem::Scheduler jobs({.threads = 3});
    std::printf("потоков: %u фоновых + ждущий = %u исполнителей\n", jobs.threads(), jobs.concurrency());

    // 1. Независимые задачи под одним счётчиком. Каждая пишет в свой элемент — синхронизация не нужна.
    std::vector<long long> sums(8);
    JobSystem::JobCounter done;
    for (std::size_t i = 0; i < sums.size(); ++i) {
        jobs.run(done, [&sums, i] {
            long long s = 0;
            for (long long k = 0; k < 1'000'000; ++k) s += k % static_cast<long long>(i + 2);
            sums[i] = s;
        });
    }
    jobs.wait(done); // ждущий поток тоже выполняет задачи
    std::printf("1. суммы: %lld … %lld\n", sums.front(), sums.back());

    // 2. TaskGroup: счётчик + ожидание в деструкторе. Задачи порождают подзадачи и ждут их.
    std::atomic<int> leaves{0};
    {
        JobSystem::TaskGroup level(jobs);
        for (int region = 0; region < 4; ++region) {
            level.run([&] {
                JobSystem::TaskGroup chunks(jobs); // вложенное ожидание не блокирует рабочий поток
                for (int c = 0; c < 16; ++c) chunks.run([&] { leaves.fetch_add(1, std::memory_order_relaxed); });
                chunks.wait();
            });
        }
        level.wait();
    }
    std::printf("2. вложенных задач выполнено: %d\n", leaves.load());

    // 3. Исключение из задачи не теряется: оно перебрасывается из wait().
    JobSystem::JobCounter risky;
    jobs.run(risky, [] { throw std::runtime_error("файл уровня не найден"); });
    jobs.run(risky, [] {});
    try {
        jobs.wait(risky);
    } catch (const std::exception& e) {
        std::printf("3. ошибка из задачи: %s\n", e.what());
    }

    // 4. Временная арена потока: выделение без блокировок, освобождение — ArenaScope.
    JobSystem::JobCounter scratchy;
    std::vector<int> zeros(4);
    for (std::size_t t = 0; t < 4; ++t) {
        jobs.run(scratchy, [&, t] {
            MemorySystem::Arena& arena = jobs.scratch();
            MemorySystem::ArenaScope scope(arena);
            std::span<int> tmp = arena.push_array<int>(1024); // нулевая память (ZII)
            zeros[t] = std::accumulate(tmp.begin(), tmp.end(), 0);
        });
    }
    jobs.wait(scratchy);
    std::printf("4. сумма нулевой временной памяти: %d\n", std::accumulate(zeros.begin(), zeros.end(), 0));

    const JobSystem::SchedulerStats stats = jobs.stats();
    std::printf("всего задач: %llu, из них выполнено ждущим потоком: %llu\n",
                static_cast<unsigned long long>(stats.jobs_executed),
                static_cast<unsigned long long>(stats.helped_while_waiting));
    return leaves.load() == 64 ? 0 : 1;
}
