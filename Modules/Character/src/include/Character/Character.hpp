#pragma once
/**
 * @file Character.hpp
 * @brief Персонаж-капсула: компоненты ECS и система движения со столкновениями по любому `Math::SdfField` (ландшафт, плоскость, планета).
 *
 * Капсула — две-три сферы по высоте. Движение: ходьба, прыжок, гравитация через `Math::gravity(pos)`.
 * При столкновении персонаж выталкивается из породы вдоль градиента SDF; физического движка нет.
 * Всё считается в Fixed и int64 — результат детерминирован.
 *
 * Мир меняется только командами: игра ставит `Motor::wish` и `Motor::jump`, остальное делает `step`.
 */

#include <ECSSystem/ECSSystem.hpp>
#include <Math/Hash.hpp>
#include <Math/Mana.hpp>
#include <Math/Vec.hpp>
#include <Math/Sdf.hpp>

#include <array>
#include <string>

namespace Character {

struct Position {
    Math::WorldPos value;    ///< Низ капсулы (под ногами).
    Math::WorldPos previous; ///< Прошлый тик — для интерполяции отрисовки.
};
struct Velocity {
    Math::FVec3 value;
};
struct Collider {
    std::array<Math::Fixed, 3> heights{}; ///< Высоты центров сфер над ногами.
    Math::Fixed radius{};
    int count = 0;
};
struct ManaPool {
    Math::Mana current{};
    Math::Mana max{};
    Math::Mana regen{}; ///< В секунду.
};
struct Grimoire {
    static constexpr int slot_count = 3;
    std::array<std::string, slot_count> slots{}; ///< Имена программ (ProgramLibrary): перечитанный файл подхватывается на лету.
    int selected = 0;
};
struct Motor {
    Math::FVec3 wish{}; ///< Желаемое направление в плоскости XZ (длина ≤ 1).
    bool jump = false;  ///< Одноразовый запрос прыжка.
    bool grounded = false;
};

struct Config {
    Math::Fixed walk_speed = Math::Fixed::from_int(5);
    Math::Fixed jump_speed = Math::Fixed::from_ratio(15, 2); ///< 7,5 м/с: высота ≈ 2,9 м — из ямы-шара радиуса 2 м можно выпрыгнуть
    Math::Fixed air_control = Math::Fixed::from_ratio(1, 8);  ///< Доля разницы скоростей, выбираемая за тик в воздухе.
    Math::Fixed terminal_speed = Math::Fixed::from_int(40);
    Math::Fixed tick = Math::Fixed::from_ratio(1, 60);
    Math::Fixed walkable_slope = Math::Fixed::from_ratio(3, 5); ///< Минимальная нормаль.y опоры.
    Math::Fixed snap_distance = Math::Fixed::from_ratio(3, 20); ///< Земля ближе этого зазора — прилипнуть (не отрываться на спуске).
    int substeps = 2;                                          ///< Шагов столкновений за тик (против «проскакивания»).
};

/// @brief Создаёт персонажа: капсула из трёх сфер r = 0,4 м, высота 1,8 м.
ECS::Entity spawn(ECS::World& world, Math::WorldPos feet, const ManaPool& mana, const Grimoire& grimoire);
/// @brief Позиция глаз (для луча прицела).
[[nodiscard]] Math::WorldPos eye(const ECS::World& world, ECS::Entity who);

/// @brief Один тик движения всех персонажей (в порядке id) и восстановление личной маны.
void step(ECS::World& world, const Math::SdfField& surface, const Config& config = {});

void hash_characters(const ECS::World& world, Math::Hasher& hasher);

} // namespace Character
