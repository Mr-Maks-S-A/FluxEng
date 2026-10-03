#pragma once
/**
 * @file SpellSim.hpp
 * @brief Симуляция «Первого заклинания»: одна цепочка «персонаж → заклинание → мана → земля» без окна и графики.
 *
 * Симуляция владеет миром (ландшафт, поле маны, ECS с персонажем и заклинаниями, генератор с сидом) и шагает
 * 60 раз в секунду. Порядок внутри тика фиксирован:
 *
 * 1. применить команды игрока (Move, Jump, Cast);
 * 2. выполнить активные заклинания в порядке id сущностей;
 * 3. применить правки ландшафта (заклинания только ставят их в очередь);
 * 4. шагнуть поле маны (каждый шестой тик — 10 раз в секунду);
 * 5. движение и столкновения;
 * 6. разослать события и поставить изменённые чанки в очередь на перестройку меша (`Terrain::SdfWorld::take_dirty`).
 *
 * Фазы — это `Phases::Schedule`: список именованных шагов, а не вызовы в одной функции. Новый модуль подключается
 * фазой (`schedule().insert_after("movement", "machines", fn)`), замеры по фазам собираются расписанием.
 *
 * Только целые и Fixed. Мир меняется только командами: запись `Replay::Session` (сид + команды) однозначно
 * восстанавливает прогон, `hashes()` — контрольная сумма ландшафта, поля маны и ECS.
 */

#include <Character/Character.hpp>
#include <Math/Math.hpp>
#include <Phases/Schedule.hpp>
#include <ManaField/ManaField.hpp>
#include <Replay/Replay.hpp>
#include <Runes/Runes.hpp>
#include <Terrain/Terrain.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace SpellSim {

constexpr int ticks_per_second = 60;
constexpr int mana_step_period = 6; ///< Поле шагает каждый шестой тик.

// ---- Команды ----

enum class CommandType : std::uint16_t { Move = 1, Jump = 2, Cast = 3 };

/// Типизированные команды: поля с именами вместо `x, y, z`. `encode` кладёт их в `Replay::Command`, `decode` читает обратно.
/// Схемы (`register_commands`) попадают в файл записи, поэтому он читается инструментами без знания игры.

/// @brief Move: направление в плоскости, квантованное в Fixed (длина ≤ 1).
struct MoveCommand {
    Math::Fixed dx{}, dz{};
    [[nodiscard]] Replay::Command encode() const noexcept;
    [[nodiscard]] static MoveCommand decode(const Replay::Command& c) noexcept;
};
struct JumpCommand {
    [[nodiscard]] Replay::Command encode() const noexcept;
};
/// @brief Cast: слот гримуара 0…2, источник маны и направление взгляда в Fixed (квантуется один раз, при создании команды).
struct CastCommand {
    int slot = 0;
    Runes::ManaSource source = Runes::ManaSource::Personal;
    Math::FVec3 aim{};
    [[nodiscard]] Replay::Command encode() const noexcept;
    [[nodiscard]] static CastCommand decode(const Replay::Command& c) noexcept;
};

/// @brief Регистрирует схемы всех команд игры (имена полей и их тип).
void register_commands(Replay::CommandRegistry& registry);

[[nodiscard]] inline Replay::Command move_command(Math::Fixed dx, Math::Fixed dz) noexcept { return MoveCommand{dx, dz}.encode(); }
[[nodiscard]] inline Replay::Command jump_command() noexcept { return JumpCommand{}.encode(); }
[[nodiscard]] inline Replay::Command cast_command(int slot, Runes::ManaSource source, Math::FVec3 aim) noexcept { return CastCommand{slot, source, aim}.encode(); }

// ---- События ----

/// @brief Ландшафт изменён: границы области в отсчётах и число чанков, чьи сетки устарели.
struct TerrainEditedEvent {
    std::int32_t lo_x = 0, lo_y = 0, lo_z = 0;
    std::int32_t hi_x = 0, hi_y = 0, hi_z = 0;
    std::int32_t chunks = 0;
    static constexpr std::string_view event_name = "spellsim.terrain_edited";
    using fields = EventSystem::Fields<
        EventSystem::Field<"lo_x", &TerrainEditedEvent::lo_x>, EventSystem::Field<"lo_y", &TerrainEditedEvent::lo_y>,
        EventSystem::Field<"lo_z", &TerrainEditedEvent::lo_z>, EventSystem::Field<"hi_x", &TerrainEditedEvent::hi_x>,
        EventSystem::Field<"hi_y", &TerrainEditedEvent::hi_y>, EventSystem::Field<"hi_z", &TerrainEditedEvent::hi_z>,
        EventSystem::Field<"chunks", &TerrainEditedEvent::chunks>>;
};

struct Config {
    std::uint64_t seed = 1;
    ManaField::Config mana{};
    Runes::Tuning runes{};
    Character::Config character{};
    Math::Mana player_mana = Math::Mana::from_int(1000);            ///< Максимум личной маны.
    Math::Mana player_mana_regen = Math::Mana::from_int(20);    ///< В секунду, сама и с постоянной скоростью.
    std::array<std::string, Character::Grimoire::slot_count> grimoire{"carve", "raise", "runaway"};
};

class Simulation {
public:
    explicit Simulation(const Config& config = {});
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    /// @brief Объявляет модули и события в шине (Runes, SpellSim). Без вызова события не публикуются.
    void declare(EventSystem::EventBus& bus);

    /// @brief Перечитывает каталог заклинаний `*.rune`: без перезапуска; идущие заклинания не страдают.
    Runes::ProgramLibrary::Report reload_spells(const std::filesystem::path& directory);

    /// @brief Один тик: команды этого тика → шесть фаз.
    void tick(std::span<const Replay::Command> commands);

    [[nodiscard]] std::uint32_t tick_number() const noexcept { return m_tick; }
    /// @brief Хеши ландшафта, поля маны и ECS (персонаж, заклинания, генератор, номер тика).
    [[nodiscard]] Replay::StateHashes hashes() const;

    // Состояние мира наружу — только для чтения: меняется оно командами внутри тика (детерминизм и запись не обойти
    // случайным вызовом из игры). Единственные «двери»: команды в `tick`, `take_dirty_chunks`, `reload_spells`,
    // `programs()` (библиотека заклинаний — данные, не состояние) и настройка гримуара.
    [[nodiscard]] const Terrain::SdfWorld& terrain() const noexcept { return m_terrain; }
    [[nodiscard]] const ManaField::ManaGrid& mana() const noexcept { return m_mana; }
    [[nodiscard]] const ECS::World& world() const noexcept { return m_world; }
    [[nodiscard]] ECS::Entity player() const noexcept { return m_player; }
    [[nodiscard]] const Runes::SpellSystem& spells() const noexcept { return m_spells; }
    [[nodiscard]] Runes::ProgramLibrary& programs() noexcept { return m_programs; }
    [[nodiscard]] const Config& config() const noexcept { return m_config; }

    /// @brief Чанки ландшафта, чьи сетки нужно перестроить (фаза 6); очередь очищается. Забирает отрисовка после тика.
    [[nodiscard]] std::vector<Terrain::ChunkCoord> take_dirty_chunks() { return m_terrain.take_dirty(); }
    /// @brief Ставит заклинание в слот гримуара по имени из библиотеки (до начала прогона; в записи это часть настройки, не команда).
    void set_grimoire_slot(int slot, std::string name);

    /// @brief Расписание фаз: порядок, замеры (`times()`), точка подключения новых фаз.
    [[nodiscard]] Phases::Schedule& schedule() noexcept { return m_schedule; }
    [[nodiscard]] const Phases::Schedule& schedule() const noexcept { return m_schedule; }
    /// @brief Сумма времён фаз последнего тика, мс (только для оверлея).
    [[nodiscard]] double tick_ms() const noexcept { return m_schedule.total_ms(); }
    [[nodiscard]] std::uint32_t casts() const noexcept { return m_casts; }
    [[nodiscard]] std::uint32_t failed_casts() const noexcept { return m_failed_casts; }
    [[nodiscard]] std::uint32_t edits_applied() const noexcept { return m_edits_applied; }

private:
    class Host;
    void apply(const Replay::Command& command);
    void build_schedule();
    void phase_commands();
    void phase_spells();
    void phase_terrain();
    void phase_mana();
    void phase_movement();
    void phase_events();

    Config m_config;
    Terrain::SdfWorld m_terrain;
    ManaField::ManaGrid m_mana;
    ECS::World m_world;
    ECS::Entity m_player;
    Runes::SpellSystem m_spells;
    Runes::ProgramLibrary m_programs;
    Math::Rng m_rng;
    Runes::EffectBuffer m_effects; ///< Эффекты заклинаний текущего тика (фаза 2 пишет, фаза 3 применяет).
    std::unique_ptr<Runes::SpellHost> m_host;
    EventSystem::EventWriter<TerrainEditedEvent> m_terrain_edited;
    bool m_declared = false;
    std::uint32_t m_tick = 0;
    std::uint32_t m_casts = 0, m_failed_casts = 0, m_edits_applied = 0;
    Phases::Schedule m_schedule;
    std::span<const Replay::Command> m_commands; ///< Команды тика, пока идёт `tick()`.
    std::vector<Terrain::EditResult> m_edited;   ///< Правки ландшафта этого тика (для событий фазы 6).
};

} // namespace SpellSim
