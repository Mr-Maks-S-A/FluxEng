/**
 * @file 02_parallel_for.cpp
 * @brief parallel_for и parallel_reduce: движение частиц, сравнение времени 0 и N потоков.
 */

#include <JobSystem/JobSystem.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

struct Particles {
    std::vector<float> x, y, vx, vy;
    explicit Particles(std::size_t n) : x(n), y(n), vx(n), vy(n) {
        for (std::size_t i = 0; i < n; ++i) {
            vx[i] = std::cos(static_cast<float>(i) * 0.37f);
            vy[i] = std::sin(static_cast<float>(i) * 0.11f);
        }
    }
    std::size_t size() const { return x.size(); }
};

// Шаг «тяжёлой» системы: каждый элемент читает и пишет только себя — идеальный кандидат для parallel_for.
void step(JobSystem::Scheduler& jobs, Particles& p, float dt) {
    JobSystem::parallel_for(jobs, p.size(), 4096, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const float ax = -p.x[i] * 0.5f, ay = -p.y[i] * 0.5f; // пружина к центру
            p.vx[i] += ax * dt;
            p.vy[i] += ay * dt;
            p.x[i] += p.vx[i] * dt + 0.001f * std::sin(p.y[i]);
            p.y[i] += p.vy[i] * dt + 0.001f * std::cos(p.x[i]);
        }
    });
}

double energy(JobSystem::Scheduler& jobs, const Particles& p) {
    return JobSystem::parallel_reduce(
        jobs, p.size(), 4096, 0.0,
        [&](std::size_t begin, std::size_t end) {
            double e = 0.0;
            for (std::size_t i = begin; i < end; ++i) e += 0.5 * (p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i]);
            return e;
        },
        [](double a, double b) { return a + b; });
}

struct Result {
    double ms;
    double energy;
};

Result simulate(unsigned threads) {
    JobSystem::Scheduler jobs({.threads = threads});
    Particles particles(1'000'000);
    const auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 30; ++tick) step(jobs, particles, 1.0f / 60.0f);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return {ms / 30.0, energy(jobs, particles)};
}

} // namespace

int main() {
    const Result serial = simulate(0);
    const unsigned threads = JobSystem::default_threads();
    const Result parallel = simulate(threads);
    std::printf("1 000 000 частиц, 30 тиков\n");
    std::printf("  0 фоновых потоков: %6.2f мс/тик, энергия %.6f\n", serial.ms, serial.energy);
    std::printf("%3u фоновых потоков: %6.2f мс/тик, энергия %.6f  (ускорение x%.1f)\n", threads, parallel.ms,
                parallel.energy, serial.ms / parallel.ms);
    // Каждый элемент считается одной и той же формулой, а свёртка идёт в порядке кусков — итог совпадает побитово.
    const bool same = serial.energy == parallel.energy;
    std::printf("результаты %s\n", same ? "совпадают побитово" : "РАЗЛИЧАЮТСЯ");
    return same ? 0 : 1;
}
