#pragma once
/**
 * @file Level.hpp
 * @brief Уровень — цель, ограничения и мир: что нужно сделать заклинаниями, чем можно воспользоваться и что считается хорошо.
 *
 * Симуляция сама по себе — сцена без цели. Уровень добавляет то, что заставляет **рисовать заклинание**: мир, в котором
 * просто так не пройти (стена, ров, толща породы), цель (дойти, пробить, насыпать), пределы (время, касты, мана, длина
 * программы) и планки «хорошо» (звёзды). Всё это — данные: уровень не содержит кода и не меняет правил симуляции, он только
 * задаёт её `Config` (сид, правки ландшафта поверх сида, точка появления, стартовые заклинания) и то, как судить исход.
 *
 * ```
 *   Level ──config──► SpellSim::Simulation      мир
 *     │ goal, limits, par
 *     └────────────► Referee ◄── observe(sim) после каждого тика ──► Status / Metrics / звёзды
 * ```
 * Уровень задаёт всю настройку прогона: чтобы повтор записи был точным, достаточно знать **номер уровня** и файл записи
 * (так же, как гримуар — часть настройки, а не команда).
 */

#include <SpellSim/SpellSim.hpp>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Challenge {

/// @brief Что нужно сделать.
enum class GoalKind : std::uint8_t {
    Reach, ///< Маг в пределах `radius` от точки.
    Carve, ///< В точке — пустота (порода вырезана).
    Raise, ///< В точке — порода (насыпано).
};

struct Goal {
    GoalKind kind = GoalKind::Reach;
    Math::WorldPos point{};
    Math::Fixed radius = Math::Fixed::from_int(2); ///< Reach: допуск по расстоянию.
};

/// @brief Жёсткие пределы: нарушил — проиграл. Ноль — «без предела».
struct Limits {
    std::uint32_t max_ticks = 60 * 60;         ///< Время, тиков (60 = секунда).
    std::uint32_t max_casts = 0;               ///< Сколько раз можно кастовать.
    std::uint32_t max_program_runes = 0;       ///< Самая длинная программа в гримуаре.
    std::int32_t max_mana_spent = 0;           ///< Суммарные траты личной маны, целых единиц.
};

/// @brief Планки на звёзды сверх победы: ★ — пройти; ★★ — потратить маны не больше; ★★★ — и программа не длиннее.
struct Par {
    std::int32_t mana_spent = 0;
    std::uint32_t program_runes = 0;
};

struct Level {
    std::string id;
    std::string title;
    std::string brief;                         ///< Что делать и чему учит уровень, одной-двумя фразами.
    SpellSim::Config config;                   ///< Мир: сид, правки поверх сида, точка появления, слоты гримуара.
    std::vector<std::pair<std::string, std::string>> library; ///< Заклинания, доступные с начала: (имя, текст `.rune`).
    Goal goal;
    Limits limits;
    Par par;
    bool needs_spell = true;                   ///< Без заклинаний пройти нельзя (проверяется тестами: «ничего не делать» проигрывает).
};

/**
 * @brief Решение: текстовые программы по слотам (уходят командой `SetProgram`) плюс ввод — сценарием по тикам и/или «ботом».
 *
 * Бот (`policy`) видит мир только для чтения и перед каждым тиком дописывает команды: так решение подстраивается под
 * рельеф (целится лучом, ждёт ману), а не опирается на «магические» числа. Играют они одинаково, как человек: командами.
 */
struct Solution {
    struct Step {
        std::uint32_t tick = 0;
        Replay::Command command;
    };
    using Policy = std::function<void(const SpellSim::Simulation&, std::vector<Replay::Command>&)>;
    std::vector<std::pair<int, std::string>> programs; ///< (слот, текст) — ставятся на тике 0.
    std::vector<Step> steps;
    Policy policy;
};

/// @brief Встроенные уровни по нарастанию сложности.
[[nodiscard]] std::span<const Level> campaign();
[[nodiscard]] const Level* find_level(std::string_view id);
/// @brief Эталонное решение встроенного уровня: доказывает, что уровень проходим, и служит демонстрацией.
[[nodiscard]] const Solution* reference_solution(std::string_view id);

/// @brief Кладёт стартовые заклинания уровня в библиотеку симуляции (игра вызывает один раз при создании мира).
void install_library(const Level& level, SpellSim::Simulation& sim);

/// @brief Хеш настройки уровня (мир + цель + пределы): одинаков у всех игроков одной партии — его сверяет `Net` при знакомстве.
[[nodiscard]] std::uint64_t level_hash(const Level& level);

} // namespace Challenge
