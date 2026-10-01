/**
 * @file 03_deterministic_output.cpp
 * @brief ChunkBuffers: параллельная система порождает события, итог побитово одинаков при любом числе потоков.
 *
 * Сценарий: башни ищут ближайшую цель и «стреляют». Поиск — параллельный, по кускам башен.
 * Выстрелы кусок пишет в свой буфер; затем один поток применяет урон в порядке кусков.
 * Порядок применения урона важен (кто добил врага — тот получает награду), и он не зависит
 * от того, какой поток какой кусок посчитал.
 */

#include <JobSystem/JobSystem.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Shot {
    std::uint32_t tower;
    std::uint32_t target;
    float damage;
};

std::uint32_t hash(std::uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

std::uint64_t battle(unsigned threads) {
    JobSystem::Scheduler jobs({.threads = threads});
    constexpr std::uint32_t towers = 20'000, enemies = 50'000;
    std::vector<float> enemy_x(enemies), enemy_hp(enemies, 10.0f), tower_x(towers);
    for (std::uint32_t i = 0; i < enemies; ++i) enemy_x[i] = static_cast<float>(hash(i) % 100'000);
    for (std::uint32_t i = 0; i < towers; ++i) tower_x[i] = static_cast<float>(hash(i + 7'777'777) % 100'000);

    JobSystem::ChunkBuffers<Shot> shots;
    std::vector<std::uint32_t> kills_by_tower(towers);
    std::uint64_t checksum = 1469598103934665603ULL;
    std::size_t total_shots = 0;

    for (std::uint32_t round = 0; round < 5; ++round) {
        constexpr std::size_t grain = 256;
        shots.reset(JobSystem::chunk_count(towers, grain));
        // Параллельно: каждая башня читает общих врагов (только чтение) и пишет выстрел в буфер своего куска.
        JobSystem::parallel_for(jobs, towers, grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t t = begin; t < end; ++t) {
                // Окно поиска — 64 «соседа» по хешу: имитация запроса к пространственной сетке.
                std::uint32_t best = UINT32_MAX;
                float best_d = 1e30f;
                for (std::uint32_t k = 0; k < 64; ++k) {
                    const std::uint32_t e = hash(static_cast<std::uint32_t>(t) * 64 + k + round) % enemies;
                    if (enemy_hp[e] <= 0.0f) continue;
                    const float d = enemy_x[e] > tower_x[t] ? enemy_x[e] - tower_x[t] : tower_x[t] - enemy_x[e];
                    if (d < best_d) best_d = d, best = e;
                }
                if (best != UINT32_MAX) shots[chunk].push_back({static_cast<std::uint32_t>(t), best, 4.0f});
            }
        });
        // Последовательно, в порядке кусков: урон, добивания, контрольная сумма.
        shots.for_each([&](const Shot& s) {
            if (enemy_hp[s.target] <= 0.0f) return; // уже добит предыдущим выстрелом — порядок важен
            enemy_hp[s.target] -= s.damage;
            if (enemy_hp[s.target] <= 0.0f) ++kills_by_tower[s.tower];
            checksum = (checksum ^ (s.tower * 2654435761ULL + s.target)) * 1099511628211ULL;
        });
        total_shots += shots.total();
    }
    std::uint64_t kills = 0;
    for (std::uint32_t t = 0; t < towers; ++t) {
        kills += kills_by_tower[t];
        checksum = (checksum ^ kills_by_tower[t]) * 1099511628211ULL;
    }
    std::printf("  %u фоновых потоков: выстрелов %zu, убито %llu, сумма %016llx\n", threads, total_shots,
                static_cast<unsigned long long>(kills), static_cast<unsigned long long>(checksum));
    return checksum;
}

} // namespace

int main() {
    const std::uint64_t reference = battle(0);
    bool same = true;
    for (const unsigned threads : {1u, 3u, 7u}) {
        same = same && battle(threads) == reference;
    }
    std::printf("итог %s\n", same ? "одинаков при любом числе потоков" : "РАЗЛИЧАЕТСЯ");
    return same ? 0 : 1;
}
