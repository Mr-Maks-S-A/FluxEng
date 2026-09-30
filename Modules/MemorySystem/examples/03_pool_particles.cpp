/**
 * @example 03_pool_particles.cpp
 * Pool: частицы рождаются и умирают по одной, каждая новая — нулевая.
 */

#include <MemorySystem/MemorySystem.hpp>

#include <print>
#include <vector>

namespace ms = MemorySystem;

struct Particle {
    float x, y;
    float vx, vy;
    float life; // 0 — мёртвая
};

int main() {
    auto pool = ms::Pool<Particle>::reserve(1024);
    std::vector<Particle*> alive;

    for (int tick = 0; tick < 60; ++tick) {
        // Рождаем три частицы за тик.
        for (int i = 0; i < 3; ++i) {
            Particle* p = pool.allocate(); // уже нулевая: x = y = vx = vy = 0
            p->vx = static_cast<float>(i) - 1.0f;
            p->vy = 2.0f;
            p->life = 0.5f;
            alive.push_back(p);
        }
        // Двигаем и хороним.
        for (std::size_t i = 0; i < alive.size();) {
            Particle* p = alive[i];
            p->x += p->vx / 30.0f;
            p->y += p->vy / 30.0f;
            p->life -= 1.0f / 30.0f;
            if (p->life <= 0.0f) {
                pool.free(p); // блок обнулён и снова в списке
                alive[i] = alive.back();
                alive.pop_back();
            } else {
                ++i;
            }
        }
    }
    std::println("after 60 ticks: {} particles alive, pool live = {}, capacity = {}", alive.size(), pool.live(),
                 pool.capacity());
}
