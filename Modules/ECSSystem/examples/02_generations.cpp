/**
 * @example 02_generations.cpp
 * Зачем поколение: ссылка на погибшую сущность не «перескакивает» на новую в том же слоте.
 */

#include <ECSSystem/ECSSystem.hpp>

#include <print>

struct Target {
    ECS::Entity entity; // Entity{} — «цели нет»
};
struct Health {
    int value = 3;
};

int main() {
    ECS::World world;

    const ECS::Entity fox = world.create();
    const ECS::Entity rabbit = world.create();
    world.emplace<Health>(rabbit);
    world.emplace<Target>(fox, rabbit); // лиса запомнила зайца

    // Заяц умер, и в его слоте родился новый.
    world.destroy(rabbit);
    const ECS::Entity newborn = world.create();
    world.emplace<Health>(newborn, 10);
    std::println("old rabbit {{{}, gen {}}}, newborn {{{}, gen {}}} — same slot, new generation", rabbit.index,
                 rabbit.generation, newborn.index, newborn.generation);

    // Лиса пытается атаковать по старой ссылке.
    const Target* target = world.get<Target>(fox);
    if (Health* h = world.get<Health>(target->entity)) {
        h->value -= 1;
        std::println("attack hit something (this would be a bug)");
    } else {
        std::println("attack ignored: target is gone (valid = {})", world.valid(target->entity));
    }
    std::println("newborn health is untouched: {}", world.get<Health>(newborn)->value);
}
