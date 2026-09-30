/**
 * @example 01_basics.cpp
 * Сущности, компоненты, выборки: минимальный цикл ECS.
 */

#include <ECSSystem/ECSSystem.hpp>

#include <print>

struct Position {
    float x = 0.0f, y = 0.0f;
};
struct Velocity {
    float x = 0.0f, y = 0.0f;
};
struct Name {
    const char* text = "unnamed";
};

int main() {
    ECS::World world;

    const ECS::Entity rabbit = world.create();
    world.emplace<Position>(rabbit, 0.0f, 0.0f);
    world.emplace<Velocity>(rabbit, 1.0f, 0.5f);
    world.emplace<Name>(rabbit, "rabbit");

    const ECS::Entity rock = world.create();
    world.emplace<Position>(rock, 10.0f, 10.0f);
    world.emplace<Name>(rock, "rock");

    // Система движения: только сущности с Position и Velocity (камень не попадёт).
    for (int tick = 0; tick < 3; ++tick) {
        world.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
            p.x += v.x;
            p.y += v.y;
        });
    }

    world.view<const Name, const Position>().each([](ECS::Entity e, const Name& n, const Position& p) {
        std::println("{:<6} entity {{{}, gen {}}} at ({:.1f}, {:.1f})", n.text, e.index, e.generation, p.x, p.y);
    });
    std::println("alive: {}, with Velocity: {}", world.alive(), world.count<Velocity>());
}
