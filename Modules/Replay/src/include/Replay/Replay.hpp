#pragma once
/**
 * @file Replay.hpp
 * @brief Запись и повтор команд игрока, журнал последних тиков (FlightRecorder).
 *
 * Мир меняется только командами внутри тика, поэтому сид + команды каждого тика полностью задают прогон.
 * `Session` прячет режимы за одним вызовом `begin_tick`:
 *
 * @code
 * Replay::Session session = Replay::Session::from_args(args, default_seed);   // --record f | --replay f | --seed N
 * // каждый тик:
 * std::span<const Replay::Command> cmds = session.begin_tick(tick, live_commands);
 * sim.step(cmds);
 * // в конце:
 * Replay::Verdict v = session.finish(ticks_run, hashes);   // запись → файл; повтор → сравнение хешей
 * @endcode
 *
 * Модуль не знает, что значат команды: тип и три числа — на совести игры.
 */

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Replay {

/// @brief Команда игрока: тип (по таблице игры), малый аргумент и три числа (Fixed.raw направления и т.п.).
struct Command {
    std::uint16_t type = 0;
    std::int16_t arg = 0;
    std::int32_t x = 0, y = 0, z = 0;
    [[nodiscard]] friend constexpr bool operator==(const Command&, const Command&) noexcept = default;
};
static_assert(sizeof(Command) == 16);

/// @brief Хеши состояний симуляции: ландшафт, поле маны, ECS (смысл ячеек — по договорённости игры).
struct StateHashes {
    std::array<std::uint64_t, 3> value{};
    [[nodiscard]] friend constexpr bool operator==(const StateHashes&, const StateHashes&) noexcept = default;
};

/// @brief Как читать одно из четырёх числовых полей команды (`arg`, `x`, `y`, `z`).
struct CommandField {
    enum class Kind : std::uint8_t { Int, Fixed }; ///< Fixed — Q16.16, показывается десятичным числом.
    std::string name;                               ///< Пусто — поле не используется.
    Kind kind = Kind::Int;
    [[nodiscard]] friend bool operator==(const CommandField&, const CommandField&) = default;
};

/// @brief Описание типа команды: имя и смысл полей. Делает запись самоописываемой.
struct CommandSchema {
    std::uint16_t type = 0;
    std::string name;
    std::array<CommandField, 4> fields{}; ///< Порядок: arg, x, y, z.
    [[nodiscard]] friend bool operator==(const CommandSchema&, const CommandSchema&) = default;
};

/// @brief Реестр схем команд игры (как реестр событий в EventSystem). Игра заполняет его один раз при старте.
class CommandRegistry {
public:
    /// @brief Регистрирует тип. Повторный `type` — нарушение контракта (FLUX_ASSERT).
    CommandRegistry& add(CommandSchema schema);
    [[nodiscard]] const CommandSchema* find(std::uint16_t type) const noexcept;
    [[nodiscard]] const std::vector<CommandSchema>& schemas() const noexcept { return m_schemas; }
    /// @brief «move dx=1 dz=0» по схеме; неизвестный тип — «type#7 arg=… x=… y=… z=…».
    [[nodiscard]] std::string format(const Command& command) const;

private:
    std::vector<CommandSchema> m_schemas;
};

/// @brief Запись прогона: сид, число тиков, команды по тикам и хеши на последнем тике.
class Recording {
public:
    std::uint64_t seed = 0;
    std::uint32_t tick_count = 0;
    StateHashes final_hashes{};
    std::vector<CommandSchema> schemas; ///< Схемы команд (файл самоописываем: инструменты работают без знания игры).

    /// @brief Добавляет команду тика. Тики — по неубыванию.
    void add(std::uint32_t tick, const Command& command);
    /// @brief Команды тика (пустой span, если их не было).
    [[nodiscard]] std::span<const Command> at(std::uint32_t tick) const noexcept;
    [[nodiscard]] std::size_t command_count() const noexcept { return m_commands.size(); }
    /// @brief Все команды по порядку записи и тик каждой.
    [[nodiscard]] std::span<const Command> commands() const noexcept { return m_commands; }
    [[nodiscard]] std::span<const std::uint32_t> command_ticks() const noexcept { return m_ticks; }

    /// @brief Сохраняет в бинарный файл. Текст ошибки — в `unexpected`.
    [[nodiscard]] std::expected<void, std::string> save(const std::string& path) const;
    [[nodiscard]] static std::expected<Recording, std::string> load(const std::string& path);

    [[nodiscard]] friend bool operator==(const Recording&, const Recording&) noexcept = default;

private:
    std::vector<std::uint32_t> m_ticks;
    std::vector<Command> m_commands;
};

/// @brief Итог сессии: для повтора — совпали ли хеши с записанными.
struct Verdict {
    bool checked = false; ///< Повтор: хеши сравнивались.
    bool match = true;    ///< Хеши совпали (или сравнивать не с чем).
    StateHashes expected{};
    StateHashes actual{};
};

/// @brief Режим прогона: без записи, запись или повтор.
class Session {
public:
    enum class Mode : std::uint8_t { Off, Record, Replay };

    /// @brief Разбирает `--record <файл>`, `--replay <файл>`, `--seed N`. Ошибка — файл не прочитан.
    [[nodiscard]] static std::expected<Session, std::string> from_args(std::span<const std::string> args, std::uint64_t default_seed,
                                                                       const CommandRegistry* registry = nullptr);
    [[nodiscard]] static Session off(std::uint64_t seed) { return Session(Mode::Off, seed); }
    /// @brief Запись; схемы из `registry` попадут в файл.
    [[nodiscard]] static Session record(std::uint64_t seed, std::string path = {}, const CommandRegistry* registry = nullptr);
    [[nodiscard]] static Session replay(Recording recording);

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return m_recording.seed; }
    /// @brief Повтор: все записанные тики исполнены.
    [[nodiscard]] bool finished(std::uint32_t tick) const noexcept { return m_mode == Mode::Replay && tick >= m_recording.tick_count; }
    [[nodiscard]] std::uint32_t recorded_ticks() const noexcept { return m_recording.tick_count; }

    /// @brief Команды, которые нужно применить на этом тике: при повторе — записанные, иначе `live` (и запись).
    [[nodiscard]] std::span<const Command> begin_tick(std::uint32_t tick, std::span<const Command> live);
    /// @brief Заканчивает прогон: запись сохраняется в файл (если путь задан), повтор сверяет хеши.
    [[nodiscard]] std::expected<Verdict, std::string> finish(std::uint32_t ticks_run, const StateHashes& hashes);

    [[nodiscard]] const Recording& recording() const noexcept { return m_recording; }

private:
    Session(Mode mode, std::uint64_t seed);

    Mode m_mode;
    Recording m_recording;
    std::string m_path;
};

/// @brief Первое расхождение двух записей.
struct Difference {
    enum class Kind : std::uint8_t { Seed, TickCount, Command, CommandCount, FinalHash };
    Kind kind = Kind::Seed;
    std::uint32_t tick = 0; ///< Для Command: тик расхождения.
    std::string text;       ///< Человекочитаемо, команды — по схемам записи `a`.
};

/// @brief Текст записи целиком: заголовок, схемы команд, команды по тикам. Не требует знания игры.
[[nodiscard]] std::string inspect(const Recording& recording, std::size_t max_commands = static_cast<std::size_t>(-1));
/// @brief Первое расхождение (сид, команды по порядку, число тиков, хеши) или `nullopt`, если записи совпадают по смыслу.
[[nodiscard]] std::optional<Difference> diff(const Recording& a, const Recording& b);

/// @brief Текст итога для консоли: «recorded N ticks…» / «replay: hashes MATCH|DIFFER FROM…».
[[nodiscard]] std::string describe(const Verdict& verdict, Session::Mode mode, std::uint32_t ticks, std::size_t commands);

/// @brief Кольцо последних тиков: номер, команды, хеши. При сбое FLUX_ASSERT сбрасывается в файл.
class FlightRecorder {
public:
    static constexpr std::size_t max_commands = 8; ///< Сколько команд тика помнить (остальные отбрасываются).

    struct TickRecord {
        std::uint32_t tick = 0;
        std::uint32_t command_count = 0; ///< Сколько команд было на самом деле.
        std::array<Command, max_commands> commands{};
        StateHashes hashes{};
    };

    explicit FlightRecorder(std::size_t capacity = 256) : m_ring(capacity == 0 ? 1 : capacity) {}

    void push(std::uint32_t tick, std::span<const Command> commands, const StateHashes& hashes) noexcept;
    /// @brief Записи от старой к новой.
    [[nodiscard]] std::vector<TickRecord> snapshot() const;
    /// @brief Текстовый дамп (тик, команды, хеши); `false` — файл не открылся.
    bool dump(const std::string& path) const;

    /// @brief Делает рекордер «аварийным»: при FLUX_ASSERT дамп уходит в `path`. Один на процесс.
    void install_assert_dump(std::string path);
    /// @brief Снимает аварийный дамп (вызывать до уничтожения рекордера).
    void uninstall_assert_dump() noexcept;

private:
    std::vector<TickRecord> m_ring;
    std::size_t m_next = 0;
    std::size_t m_size = 0;
};

/// @brief Симуляция, которую можно записывать и повторять: тик по командам, номер тика, хеши состояния.
template<typename Sim>
concept Simulatable = requires(Sim& sim, std::span<const Command> commands) {
    sim.tick(commands);
    { sim.tick_number() } -> std::convertible_to<std::uint32_t>;
    { std::as_const(sim).hashes() } -> std::same_as<StateHashes>;
};

/**
 * @brief Склейка «сессия записи/повтора + журнал тиков + симуляция» в один вызов на тик.
 *
 * Весь код записи и повтора, который иначе пишет каждая игра:
 * @code
 * Replay::Driver driver(sim, session, &recorder);
 * // каждый тик (live — команды из ввода; при повторе игнорируются):
 * if (!driver.step(live)) quit();                 // false — повтор закончился, тик не выполнялся
 * // в конце:
 * std::println("{}", Replay::describe(*driver.finish(), session.mode(), ...));
 * @endcode
 */
template<Simulatable Sim>
class Driver {
public:
    Driver(Sim& sim, Session& session, FlightRecorder* recorder = nullptr) : m_sim(&sim), m_session(&session), m_recorder(recorder) {}

    /// @brief Один тик симуляции с командами сессии. `false` — повтор дошёл до конца записи (тик не выполнялся).
    bool step(std::span<const Command> live) {
        const std::uint32_t tick = m_sim->tick_number();
        if (m_session->finished(tick)) return false;
        const std::span<const Command> commands = m_session->begin_tick(tick, live);
        m_sim->tick(commands);
        if (m_recorder != nullptr) m_recorder->push(tick, commands, std::as_const(*m_sim).hashes());
        return true;
    }
    /// @brief Закрывает сессию: запись пишет файл, повтор сверяет хеши с записанными.
    [[nodiscard]] std::expected<Verdict, std::string> finish() { return m_session->finish(m_sim->tick_number(), std::as_const(*m_sim).hashes()); }
    /// @brief Повтор: команды берутся из записи — ввод читать не нужно.
    [[nodiscard]] bool replaying() const noexcept { return m_session->mode() == Session::Mode::Replay; }

private:
    Sim* m_sim;
    Session* m_session;
    FlightRecorder* m_recorder;
};

} // namespace Replay
