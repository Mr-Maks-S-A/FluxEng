/**
 * @example 03_systems.cpp
 * Системы как свободные функции над World: движение, старение, рождение.
 * Показывает, что можно и чего нельзя делать во время обхода.
 */

#include <ECSSystem/ECSSystem.hpp>

#include <print>
#include <vector>

struct Position {
    float x = 0.0f, y = 0.0f;
};
struct Velocity {
    float x = 0.0f, y = 0.0f;
};
struct Lifetime {
    int ticks_left = 0;
};

// Движение: только читает Velocity.
void movement_system(ECS::World& world) {
    world.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
    });
}

// Старение: уничтожать ТЕКУЩУЮ сущность внутри each можно — обход идёт с конца.
void lifetime_system(ECS::World& world) {
    world.view<Lifetime>().each([&](ECS::Entity e, Lifetime& life) {
        if (--life.ticks_left <= 0) {
            world.destroy(e);
        }
    });
}

// Рождение: создавать сущности и добавлять компоненты во время обхода нельзя —
// сначала собираем, потом применяем.
void spawn_system(ECS::World& world, int tick) {
    std::vector<Position> births;
    world.view<const Position, const Lifetime>().each([&](const Position& p, const Lifetime& life) {
        if (life.ticks_left == 2 && tick % 2 == 0) births.push_back(p);
    });
    for (const Position& at : births) {
        const ECS::Entity child = world.create();
        world.emplace<Position>(child, at);
        world.emplace<Velocity>(child, 0.0f, 1.0f);
        world.emplace<Lifetime>(child, 3);
    }
}

int main() {
    ECS::World world;
    for (int i = 0; i < 5; ++i) {
        const ECS::Entity e = world.create();
        world.emplace<Position>(e, static_cast<float>(i), 0.0f);
        world.emplace<Velocity>(e, 1.0f, 0.0f);
        world.emplace<Lifetime>(e, 2 + i);
    }

    for (int tick = 0; tick < 8; ++tick) {
        movement_system(world);
        spawn_system(world, tick);
        lifetime_system(world);
        std::println("tick {}: alive {}, slots {}", tick, world.alive(), world.registry().slots());
    }
}
