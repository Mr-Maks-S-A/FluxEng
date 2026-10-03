/**
 * @example 01_walk_on_plane.cpp
 * Персонаж-капсула: падение, ходьба, прыжок и личная мана — над плоским миром (`Math::PlaneSdf`).
 *
 * Модуль `Character` не знает, по чему идёт: ему нужен лишь интерфейс `Math::SdfField`. Ландшафт, плоскость
 * или планета подходят одинаково, поэтому этот пример не создаёт никакого мира.
 */

#include <Character/Character.hpp>

#include <algorithm>
#include <cstdio>

using namespace Character;
using Math::Fixed;
using Math::Mana;
using Math::WorldPos;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static double meters(std::int64_t raw) { return static_cast<double>(raw) / 65536.0; }

int main() {
    const Math::PlaneSdf ground(WorldPos::from_meters(0, 10, 0).y); // земля на высоте 10 м
    ECS::World world;

    // 1. Персонаж — сущность ECS с компонентами: позиция, скорость, коллайдер (3 сферы), личная мана, гримуар, мотор.
    const ECS::Entity hero = spawn(world, WorldPos::from_meters(5, 14, 5), {.current = Mana::from_int(10), .max = Mana::from_int(100), .regen = Mana::from_int(5)}, {});
    const auto& position = *world.get<Position>(hero);
    auto& motor = *world.get<Motor>(hero);

    // 2. Тик движения (60 Гц): гравитация, столкновения по полю, восстановление маны. Падаем с 4 м и встаём на землю.
    for (int tick = 0; tick < 90; ++tick) step(world, ground);
    std::printf("1. после падения: y = %.2f м (земля 10.00), стоит на земле: %s\n", meters(position.value.y), motor.grounded ? "да" : "нет");
    EXPECT(motor.grounded);
    EXPECT(std::abs(meters(position.value.y) - 10.0) < 0.05);

    // 3. Ходьба: игра ставит желаемое направление (длина ≤ 1), модуль двигает со скоростью 5 м/с.
    const double x0 = meters(position.value.x);
    motor.wish = {Fixed::from_int(1), Fixed{}, Fixed{}};
    for (int tick = 0; tick < 60; ++tick) step(world, ground);
    std::printf("2. за секунду ходьбы по +x прошёл %.2f м, высота %.2f\n", meters(position.value.x) - x0, meters(position.value.y));
    EXPECT(meters(position.value.x) - x0 > 4.5);

    // 4. Прыжок: одноразовый запрос; с земли вверх на ≈ 2.9 м (скорость 7.5 м/с).
    motor.wish = {};
    for (int tick = 0; tick < 30; ++tick) step(world, ground); // остановиться
    motor.jump = true;
    double apex = meters(position.value.y);
    for (int tick = 0; tick < 120; ++tick) {
        step(world, ground);
        apex = std::max(apex, meters(position.value.y));
    }
    std::printf("3. высота прыжка %.2f м, приземлился: %s\n", apex - 10.0, motor.grounded ? "да" : "нет");
    EXPECT(apex - 10.0 > 2.5 && apex - 10.0 < 3.2);
    EXPECT(motor.grounded);

    // 5. Личная мана восстанавливается сама, с постоянной скоростью, до максимума.
    const Mana mana = world.get<ManaPool>(hero)->current;
    std::printf("4. мана после 5 с (300 тиков): %.1f из 100 (стартовала с 10, +5/с)\n", mana.to_double());
    EXPECT(mana.to_double() > 34.0 && mana.to_double() < 36.0); // 10 + 5 · 5 с

    // 6. Хеш персонажей — часть хеша состояния симуляции (проверка детерминизма).
    Math::Hasher hasher;
    hash_characters(world, hasher);
    std::printf("5. хеш персонажей: %016llx\nOK\n", static_cast<unsigned long long>(hasher.value()));
    return 0;
}
