#include <JobSystem/Parallel.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

using JobSystem::ChunkBuffers;
using JobSystem::Scheduler;

TEST_SUITE("JobSystem.Parallel") {

TEST_CASE("chunk_count") {
    static_assert(JobSystem::chunk_count(0, 10) == 0);
    static_assert(JobSystem::chunk_count(1, 10) == 1);
    static_assert(JobSystem::chunk_count(10, 10) == 1);
    static_assert(JobSystem::chunk_count(11, 10) == 2);
    static_assert(JobSystem::chunk_count(5, 0) == 5); // grain 0 → 1
    CHECK(true);
}

TEST_CASE("parallel_for покрывает каждый индекс ровно один раз") {
    Scheduler jobs({.threads = 3});
    for (const std::size_t count : {0u, 1u, 7u, 1000u, 100003u}) {
        for (const std::size_t grain : {1u, 64u, 4096u}) {
            CAPTURE(count);
            CAPTURE(grain);
            std::vector<std::atomic<int>> hits(count);
            JobSystem::parallel_for(jobs, count, grain, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) hits[i].fetch_add(1, std::memory_order_relaxed);
            });
            bool all_once = true;
            for (auto& h : hits) all_once = all_once && h.load() == 1;
            CHECK(all_once);
        }
    }
}

TEST_CASE("номер куска и его границы не зависят от числа потоков") {
    auto boundaries = [](unsigned threads) {
        Scheduler jobs({.threads = threads});
        std::vector<std::pair<std::size_t, std::size_t>> seen(JobSystem::chunk_count(1000, 128));
        JobSystem::parallel_for(jobs, 1000, 128,
                                [&](std::size_t begin, std::size_t end, std::size_t chunk) { seen[chunk] = {begin, end}; });
        return seen;
    };
    const auto serial = boundaries(0);
    CHECK(serial.front() == std::pair<std::size_t, std::size_t>{0, 128});
    CHECK(serial.back() == std::pair<std::size_t, std::size_t>{896, 1000});
    CHECK(boundaries(1) == serial);
    CHECK(boundaries(5) == serial);
}

TEST_CASE("parallel_reduce с float даёт побитово одинаковый результат при любом числе потоков") {
    std::vector<float> values(200'001);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = 1.0f / static_cast<float>(i + 1); // порядок важен для float
    auto sum = [&](unsigned threads) {
        Scheduler jobs({.threads = threads});
        return JobSystem::parallel_reduce(
            jobs, values.size(), 1000, 0.0f,
            [&](std::size_t begin, std::size_t end) {
                float s = 0.0f;
                for (std::size_t i = begin; i < end; ++i) s += values[i];
                return s;
            },
            [](float a, float b) { return a + b; });
    };
    const float reference = sum(0);
    CHECK(sum(1) == reference);
    CHECK(sum(3) == reference);
    CHECK(sum(7) == reference);
}

TEST_CASE("ChunkBuffers: параллельная запись, слияние в порядке кусков") {
    auto collect = [](unsigned threads) {
        Scheduler jobs({.threads = threads});
        ChunkBuffers<std::uint32_t> out;
        const std::size_t count = 50'000;
        const std::size_t grain = 777;
        out.reset(JobSystem::chunk_count(count, grain));
        JobSystem::parallel_for(jobs, count, grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t i = begin; i < end; ++i) {
                if (i % 7 == 0) out[chunk].push_back(static_cast<std::uint32_t>(i));
            }
        });
        std::vector<std::uint32_t> merged;
        out.for_each([&](std::uint32_t v) { merged.push_back(v); });
        CHECK(merged.size() == out.total());
        return merged;
    };
    const auto serial = collect(0);
    CHECK(serial.size() == (50'000 + 6) / 7);
    CHECK(std::is_sorted(serial.begin(), serial.end())); // порядок кусков = порядок индексов
    CHECK(collect(3) == serial);
    CHECK(collect(8) == serial);
}

TEST_CASE("ChunkBuffers: reset сохраняет ёмкость и очищает содержимое") {
    ChunkBuffers<int> out;
    out.reset(4);
    out[2].assign(100, 1);
    const std::size_t capacity = out[2].capacity();
    out.reset(4);
    CHECK(out[2].empty());
    CHECK(out[2].capacity() == capacity);
    CHECK(out.total() == 0);
    out.reset(2);
    CHECK(out.chunks() == 2);
}

TEST_CASE("вложенный parallel_for не блокируется") {
    Scheduler jobs({.threads = 2});
    std::atomic<int> cells{0};
    JobSystem::parallel_for(jobs, 16, 1, [&](std::size_t, std::size_t) {
        JobSystem::parallel_for(jobs, 64, 8, [&](std::size_t begin, std::size_t end) {
            cells.fetch_add(static_cast<int>(end - begin), std::memory_order_relaxed);
        });
    });
    CHECK(cells.load() == 16 * 64);
}

TEST_CASE("исключение из куска доходит до вызывающего") {
    for (const unsigned threads : {0u, 3u}) {
        CAPTURE(threads);
        Scheduler jobs({.threads = threads});
        CHECK_THROWS_AS(JobSystem::parallel_for(jobs, 1000, 10,
                                                [&](std::size_t begin, std::size_t) {
                                                    if (begin == 500) throw std::logic_error("bad chunk");
                                                }),
                        std::logic_error);
    }
}

}
