#include <JobSystem/Scheduler.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using JobSystem::JobCounter;
using JobSystem::Scheduler;
using JobSystem::TaskGroup;

TEST_SUITE("JobSystem.Scheduler") {

TEST_CASE("JobCounter{} — «ничего не запущено» (ZII)") {
    Scheduler jobs({.threads = 2});
    JobCounter counter;
    CHECK(counter.done());
    jobs.wait(counter); // возвращается сразу
}

TEST_CASE("все задачи выполняются ровно один раз") {
    for (const unsigned threads : {0u, 1u, 3u, 8u}) {
        CAPTURE(threads);
        Scheduler jobs({.threads = threads});
        CHECK(jobs.concurrency() == threads + 1);
        std::atomic<int> sum{0};
        JobCounter counter;
        for (int i = 1; i <= 1000; ++i) jobs.run(counter, [&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
        jobs.wait(counter);
        CHECK(sum.load() == 500500);
        CHECK(counter.done());
        CHECK(jobs.stats().jobs_executed == 1000);
    }
}

TEST_CASE("без фоновых потоков задача выполняется сразу, в вызывающем потоке") {
    Scheduler jobs({.threads = 0});
    JobCounter counter;
    std::thread::id where;
    jobs.run(counter, [&] { where = std::this_thread::get_id(); });
    CHECK(counter.done()); // ещё до wait()
    CHECK(where == std::this_thread::get_id());
}

TEST_CASE("задачи действительно идут на других потоках") {
    Scheduler jobs({.threads = 3});
    std::mutex mutex;
    std::set<std::thread::id> seen;
    std::atomic<int> started{0};
    JobCounter counter;
    for (int i = 0; i < 3; ++i) {
        jobs.run(counter, [&] {
            started.fetch_add(1);
            // Ждём, пока все три задачи начнутся: значит, они идут одновременно.
            while (started.load() < 3) std::this_thread::yield();
            std::lock_guard lock(mutex);
            seen.insert(std::this_thread::get_id());
        });
    }
    jobs.wait(counter);
    CHECK(seen.size() >= 2);
}

TEST_CASE("вложенные задачи: задача ждёт свои подзадачи без взаимной блокировки") {
    Scheduler jobs({.threads = 2});
    std::atomic<int> leaves{0};
    JobCounter outer;
    for (int i = 0; i < 8; ++i) {
        jobs.run(outer, [&] {
            JobCounter inner;
            for (int k = 0; k < 16; ++k) jobs.run(inner, [&] { leaves.fetch_add(1); });
            jobs.wait(inner); // рабочий поток помогает, а не блокируется
        });
    }
    jobs.wait(outer);
    CHECK(leaves.load() == 8 * 16);
}

TEST_CASE("исключение из задачи доходит до wait, остальные задачи выполняются") {
    Scheduler jobs({.threads = 2});
    std::atomic<int> finished{0};
    JobCounter counter;
    for (int i = 0; i < 10; ++i) {
        jobs.run(counter, [&, i] {
            if (i == 3) throw std::runtime_error("job 3 failed");
            finished.fetch_add(1);
        });
    }
    CHECK_THROWS_WITH_AS(jobs.wait(counter), "job 3 failed", std::runtime_error);
    CHECK(finished.load() == 9);
    CHECK(counter.done());
    jobs.wait(counter); // ошибка уже отдана: повторное ожидание чистое
}

TEST_CASE("TaskGroup ждёт свои задачи") {
    Scheduler jobs({.threads = 2});
    std::atomic<int> value{0};
    {
        TaskGroup group(jobs);
        for (int i = 0; i < 100; ++i) group.run([&] { value.fetch_add(1); });
        group.wait();
        CHECK(value.load() == 100);
        group.run([&] { value.fetch_add(1); });
    } // деструктор дождался
    CHECK(value.load() == 101);
}

TEST_CASE("у каждого потока своя временная арена") {
    Scheduler jobs({.threads = 3});
    std::mutex mutex;
    std::map<std::thread::id, std::pair<MemorySystem::Arena*, unsigned>> seen;
    JobCounter counter;
    std::atomic<int> started{0};
    for (int i = 0; i < 4; ++i) {
        jobs.run(counter, [&] {
            started.fetch_add(1);
            while (started.load() < 4) std::this_thread::yield(); // 4 задачи одновременно: 3 рабочих + ждущий
            MemorySystem::Arena& scratch = jobs.scratch();
            MemorySystem::ArenaScope scope(scratch);
            int* value = scratch.push<int>();
            REQUIRE(value != nullptr);
            CHECK(*value == 0); // нулевая память
            std::lock_guard lock(mutex);
            seen[std::this_thread::get_id()] = {&scratch, jobs.this_thread_index()};
        });
    }
    jobs.wait(counter); // ждущий поток помогает: одна задача выполнится на нём (индекс 0)
    REQUIRE(seen.size() == 4);
    std::set<MemorySystem::Arena*> arenas;
    std::set<unsigned> indices;
    for (const auto& [id, entry] : seen) {
        arenas.insert(entry.first);
        indices.insert(entry.second);
    }
    CHECK(arenas.size() == 4);
    CHECK(indices == std::set<unsigned>{0, 1, 2, 3});
    CHECK(seen.at(std::this_thread::get_id()).first == &jobs.scratch());
}

TEST_CASE("уснувший рабочий поток просыпается на новую задачу (пробуждение не теряется)") {
    Scheduler jobs({.threads = 2});
    for (int round = 0; round < 50; ++round) {
        // Даём рабочим потокам докрутиться и уснуть.
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 5 == 0 ? 5 : 0));
        std::atomic<bool> b_ran{false};
        JobCounter counter;
        // A стоит в очереди первой — её возьмёт ждущий поток и будет ждать B.
        // B может выполнить только рабочий поток: если бы пробуждение потерялось, тест завис бы.
        jobs.run(counter, [&] {
            while (!b_ran.load()) std::this_thread::yield();
        });
        jobs.run(counter, [&] { b_ran.store(true); });
        jobs.wait(counter);
        CHECK(b_ran.load());
    }
}

TEST_CASE("деструктор останавливает потоки (много раз подряд)") {
    for (int i = 0; i < 20; ++i) {
        Scheduler jobs({.threads = 4, .scratch_bytes = MemorySystem::KiB(64)});
        JobCounter counter;
        jobs.run(counter, [] {});
        jobs.wait(counter);
    }
    CHECK(true);
}

}
