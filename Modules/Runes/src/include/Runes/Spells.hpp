#pragma once
/**
 * @file Spells.hpp
 * @brief Стековая машина рун, оплата маной, активные заклинания как сущности ECS.
 *
 * Машина: стек из Fixed глубиной 64 (вектор — три ячейки x, y, z; y лежит ближе к вершине, z на вершине),
 * за тик — не больше 256 рун, продолжение со следующего тика. Ошибка (пустой стек, переход за пределы,
 * нечем платить) останавливает заклинание и порождает `SpellFailedEvent`, движок не падает.
 *
 * Оплата: обычная руна — малая фиксированная цена, эффект — пропорционально объёму (k · r³).
 * - личный запас: вся цена списывается с мага;
 * - окружающая мана: цена списывается с поля вокруг мага (`SpellHost::draw`), с мага — в N раз меньше.
 * Если поле отдало меньше нужного, заклинание останавливается на руне, которую не смогло оплатить.
 *
 * Мир модуль не знает. Руны читают его и платят из кошелька через `SpellHost` (чувства и мана), а всё, что должно
 * измениться в мире, возвращают **данными** — буфером `Effect`. Хозяин мира применяет эффекты в своей фазе тика:
 * машина рун не меняет мир сама, поэтому её легко записывать, откатывать, параллелить и показывать в трассе.
 */

#include <Runes/Graph.hpp>
#include <Runes/Program.hpp>

#include <ECSSystem/ECSSystem.hpp>
#include <EventSystem/EventSystem.hpp>
#include <Math/Hash.hpp>
#include <Math/Mana.hpp>
#include <Math/Vec.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Runes {

constexpr std::size_t stack_depth = 64;
constexpr std::size_t max_runes_per_tick = 256;

/// @brief Константы стоимости; меняются в одном месте.
struct Tuning {
    Math::Mana rune_cost = Math::Mana::from_ratio(1, 20);        ///< Обычная руна.
    Math::Mana effect_k = Math::Mana::from_int(30);              ///< Эффект: k · r³ (маны на кубический метр).
    int ambient_divisor = 10;                                    ///< N: с мага берётся в N раз меньше цены.
    Math::Fixed ambient_radius = Math::Fixed::from_int(3);       ///< Радиус, из которого идёт оплата полем.
    Math::Mana max_budget = Math::Mana::from_int(5000);          ///< Предел суммарной цены одного заклинания.
    Math::Fixed min_radius = Math::Fixed::from_ratio(1, 4);      ///< Радиусы эффектов зажимаются в этот диапазон.
    Math::Fixed max_radius = Math::Fixed::from_int(6);
    Math::Fixed target_range = Math::Fixed::from_int(40);        ///< Дальность луча TARGET.
};

enum class ManaSource : std::uint8_t { Personal, Ambient };

enum class Failure : std::uint8_t { None, StackUnderflow, StackOverflow, JumpOutOfRange, OutOfMana, OutOfBudget, UnknownRune };
[[nodiscard]] std::string_view failure_text(Failure failure) noexcept;

enum class Status : std::uint8_t { Running, Halted, Failed };

/// @brief Что заклинание хочет изменить в мире. Данные, а не вызов: мир применяет их в своей фазе.
struct Effect {
    enum class Kind : std::uint8_t { Carve, Raise };
    Kind kind = Kind::Carve;
    ECS::Entity caster;       ///< Чьё заклинание.
    Math::WorldPos position;  ///< Центр сферы.
    Math::Fixed radius{};     ///< Уже зажатый в допустимый диапазон радиус.
};
using EffectBuffer = std::vector<Effect>;

/**
 * @brief Мир глазами заклинания: чувства (позиция, цель, плотность маны) и кошелёк (личная и окружающая мана).
 * Изменений мира здесь нет — они уходят в `EffectBuffer`.
 */
class SpellHost {
public:
    virtual ~SpellHost() = default;
    /// @brief Позиция мага (откуда читаются CASTER и оплата полем).
    [[nodiscard]] virtual Math::WorldPos position(ECS::Entity caster) = 0;
    /// @brief Точка попадания луча из глаз мага вдоль `aim` (если ничего нет — точка на дальности).
    [[nodiscard]] virtual Math::WorldPos target(ECS::Entity caster, Math::FVec3 aim, Math::Fixed range) = 0;
    [[nodiscard]] virtual Math::Mana density(Math::WorldPos pos) = 0;
    /// @brief Забирает ману из поля; возвращает, сколько забрано.
    virtual Math::Mana draw(Math::WorldPos pos, Math::Fixed radius, Math::Mana amount) = 0;
    /// @brief Списывает с мага `amount`; `false` и ничего не списано, если не хватает.
    virtual bool take_personal(ECS::Entity caster, Math::Mana amount) = 0;
    /// @brief Добавляет магу ману (руна DRAW).
    virtual void give_personal(ECS::Entity caster, Math::Mana amount) = 0;
};

// ---- Компоненты активного заклинания (сущность ECS) ----

struct SpellProgram {
    std::shared_ptr<const Program> program; ///< Ссылка на неизменную программу (перезагрузка файла не ломает идущие).
};
struct MachineState {
    std::array<Math::Fixed, stack_depth> stack{};
    std::uint32_t sp = 0;
    std::uint32_t pc = 0;
    std::uint32_t runes_executed = 0;
    Status status = Status::Running;
    Failure failure = Failure::None;
};
struct SpellBudget {
    Math::Mana remaining{}; ///< Сколько ещё можно потратить.
    Math::Mana spent{};
    ManaSource source = ManaSource::Personal;
};
struct SpellCaster {
    ECS::Entity caster;
    Math::FVec3 aim;
};

// ---- События ----

struct SpellFailedEvent {
    std::uint32_t caster = 0; ///< Индекс сущности мага.
    std::uint32_t spell = 0;  ///< Индекс сущности заклинания.
    std::uint32_t reason = 0; ///< Failure.
    std::uint32_t pc = 0;     ///< Руна, на которой остановились.
    static constexpr std::string_view event_name = "runes.spell_failed";
    using fields = EventSystem::Fields<EventSystem::Field<"caster", &SpellFailedEvent::caster>, EventSystem::Field<"spell", &SpellFailedEvent::spell>,
                                       EventSystem::Field<"reason", &SpellFailedEvent::reason>, EventSystem::Field<"pc", &SpellFailedEvent::pc>>;
};
struct SpellFinishedEvent {
    std::uint32_t caster = 0;
    std::uint32_t spell = 0;
    std::uint32_t runes = 0; ///< Выполнено рун.
    std::int32_t spent = 0;  ///< Потрачено, Fixed.raw.
    static constexpr std::string_view event_name = "runes.spell_finished";
    using fields = EventSystem::Fields<EventSystem::Field<"caster", &SpellFinishedEvent::caster>, EventSystem::Field<"spell", &SpellFinishedEvent::spell>,
                                       EventSystem::Field<"runes", &SpellFinishedEvent::runes>, EventSystem::Field<"spent", &SpellFinishedEvent::spent>>;
};

/// @brief Трасса последнего заклинания: какие руны исполнены и сколько маны потрачено.
struct SpellTrace {
    struct Entry {
        std::uint32_t pc = 0;
        Rune rune = Rune::Halt;
        Math::Mana cost{};
    };
    static constexpr std::size_t max_entries = 64; ///< Хранятся последние.

    std::string program;
    ManaSource source = ManaSource::Personal;
    std::vector<Entry> entries;       ///< Кольцо последних max_entries рун (от старых к новым после `ordered()`).
    std::uint32_t runes_executed = 0;
    std::uint32_t effects = 0;        ///< Сколько эффектов породило заклинание.
    Math::Mana spent{};
    Status status = Status::Running;
    Failure failure = Failure::None;
    std::uint32_t failed_pc = 0;
    bool valid = false;
};

/// @brief Библиотека программ из каталога: `*.rune` (текст) и `*.rungraph` (граф); имя файла без расширения — имя заклинания.
/// Оба формата попадают в один и тот же байт-код `Program`.
class ProgramLibrary {
public:
    struct Report {
        std::size_t loaded = 0;
        std::vector<std::string> errors; ///< «файл:строка: сообщение». Программа с ошибкой остаётся прежней.
    };

    /// @brief (Пере)читает каталог. Без перезапуска: идущие заклинания держат старые программы.
    Report load_directory(const std::filesystem::path& directory);
    /// @brief Добавляет программу из текста (тесты, встроенные заклинания).
    [[nodiscard]] std::expected<void, Diagnostic> add_text(std::string name, std::string_view text);

    [[nodiscard]] std::shared_ptr<const Program> find(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> names() const;

private:
    std::map<std::string, std::shared_ptr<const Program>, std::less<>> m_programs;
};

class SpellSystem {
public:
    explicit SpellSystem(const Tuning& tuning = {}) : m_tuning(tuning) {}

    /// @brief Объявляет модуль «Runes» и события в шине. Без вызова события не публикуются.
    void declare(EventSystem::EventBus& bus);

    /// @brief Создаёт сущность-заклинание. Бюджет — `Tuning::max_budget`.
    ECS::Entity cast(ECS::World& world, ECS::Entity caster, std::shared_ptr<const Program> program, Math::FVec3 aim, ManaSource source);

    /// @brief Исполняет все активные заклинания в порядке id сущностей; закончившиеся удаляет.
    /// Эффекты на мир дописываются в `effects` в порядке исполнения — применяет их хозяин мира.
    void tick(ECS::World& world, SpellHost& host, EffectBuffer& effects);

    [[nodiscard]] const SpellTrace& last_trace() const noexcept { return m_trace; }
    [[nodiscard]] const Tuning& tuning() const noexcept { return m_tuning; }
    [[nodiscard]] std::size_t active(const ECS::World& world) const { return world.count<MachineState>(); }

private:
    void run(ECS::World& world, SpellHost& host, EffectBuffer& effects, ECS::Entity spell);

    Tuning m_tuning;
    EventSystem::EventWriter<SpellFailedEvent> m_failed;
    EventSystem::EventWriter<SpellFinishedEvent> m_finished;
    bool m_declared = false;
    ECS::Entity m_trace_spell{};
    SpellTrace m_trace;
};

/// @brief Добавляет в хеш состояние всех активных заклинаний (в порядке id).
void hash_spells(const ECS::World& world, Math::Hasher& hasher);

} // namespace Runes
