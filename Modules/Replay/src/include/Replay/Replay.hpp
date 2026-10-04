#pragma once
/**
 * @file Replay.hpp
 * @brief Запись и повтор команд игрока, именованные хеши подсистем, журнал последних тиков (FlightRecorder).
 *
 * Мир меняется только командами внутри тика, поэтому сид + команды каждого тика полностью задают прогон.
 *
 * - **Хеши подсистем названы** (`StateHashes`: «terrain», «mana», «characters»…): при расхождении повтора сразу видно,
 *   какая подсистема разошлась, а не «хеши не совпали».
 * - **Запись — журнал EventLog**: команды пишутся по ходу игры и сбрасываются на диск каждые 60 тиков; повреждённые блоки
 *   восстанавливаются по чётности, а после аварийного завершения файл читается до последнего сброса (без финальных хешей).
 *   Старые файлы (версии 1 и 2) читаются.
 *
 * @code
 * Replay::Session session = Replay::Session::from_args(args, default_seed).value();   // --record f | --replay f | --seed N
 * Replay::Driver driver(sim, session);                    // sim: tick(span<Command>), tick_number(), hashes()
 * while (driver.step(live_commands)) {}                   // каждый тик; false — повтор дошёл до конца
 * auto verdict = driver.finish().value();                 // запись → трейлер с хешами; повтор → сверка по подсистемам
 * @endcode
 *
 * Модуль не знает, что значат команды: тип и три числа — на совести игры (схемы — `CommandRegistry`).
 */

#include <EventLog/Journal.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// @brief Хеш одной подсистемы: имя (до 15 символов) и значение.
/// @brief Хеш содержимого (FNV-1a, 64 бита): имя блоба. Не криптографический — защита от случайных ошибок, а не от злоумышленника.
[[nodiscard]] std::uint64_t content_hash(std::span<const std::byte> bytes) noexcept;

/**
 * @brief Блоб — произвольные данные, на которые ссылаются команды хешем (например, программа заклинания для `SetProgram`).
 *
 * Команда занимает 16 байт и не вмещает граф. Поэтому большое лежит в записи рядом, адресуется содержимым, а команда
 * несёт только хеш: одна и та же программа не дублируется, а повтор и сеть доставляют блоб раньше команды.
 */
struct Blob {
    std::uint64_t hash = 0;
    std::vector<std::byte> bytes;
    [[nodiscard]] friend bool operator==(const Blob&, const Blob&) noexcept = default;
};

struct NamedHash {
    std::array<char, 16> name{};
    std::uint64_t value = 0;
    [[nodiscard]] std::string_view name_view() const noexcept { return std::string_view(name.data()); }
    [[nodiscard]] friend constexpr bool operator==(const NamedHash&, const NamedHash&) noexcept = default;
};

/**
 * @brief Хеши состояния по подсистемам (до 8). Порядок и имена задаёт симуляция; сравнение — по именам.
 *
 * Фиксированный размер: хеши пишутся в журнал каждого тика без аллокаций.
 */
class StateHashes {
public:
    static constexpr std::size_t capacity = 8;

    /// @brief Добавляет хеш подсистемы. Имя уникально, длина ≤ 15; больше `capacity` подсистем — нарушение контракта.
    StateHashes& add(std::string_view name, std::uint64_t value);
    [[nodiscard]] std::size_t count() const noexcept { return m_count; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] std::span<const NamedHash> entries() const noexcept { return {m_entries.data(), m_count}; }
    /// @brief Значение по имени; `nullopt`, если такой подсистемы нет.
    [[nodiscard]] std::optional<std::uint64_t> find(std::string_view name) const noexcept;
    /// @brief Значение по имени; 0, если нет (для тестов и вывода).
    [[nodiscard]] std::uint64_t at(std::string_view name) const noexcept { return find(name).value_or(0); }
    /// @brief Имена подсистем, у которых значения разные или которых нет с одной из сторон.
    [[nodiscard]] std::vector<std::string> differing(const StateHashes& other) const;
    /// @brief «terrain=00ab… mana=…».
    [[nodiscard]] std::string describe() const;
    [[nodiscard]] friend bool operator==(const StateHashes& a, const StateHashes& b) noexcept { return a.differing(b).empty() && a.m_count == b.m_count; }

private:
    std::array<NamedHash, capacity> m_entries{};
    std::uint8_t m_count = 0;
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

/// @brief Что нашлось при чтении файла записи.
struct LoadInfo {
    enum class Format : std::uint8_t { Journal, Legacy }; ///< Журнал EventLog (текущий) или старый плоский файл (версии 1–2).
    Format format = Format::Journal;
    bool complete = true;                 ///< Есть трейлер (финальные хеши): запись закончена штатно.
    EventLog::RecoveryReport recovery{};  ///< Для журнала: сколько блоков испорчено и восстановлено, пропуски записей.
};

/// @brief Запись прогона: сид, число тиков, команды по тикам и хеши на последнем тике.
class Recording {
public:
    std::uint64_t seed = 0;
    std::uint32_t tick_count = 0;
    StateHashes final_hashes{};         ///< Пусто, если запись оборвана (нет трейлера).
    std::vector<CommandSchema> schemas; ///< Схемы команд (файл самоописываем: инструменты работают без знания игры).
    bool complete = true;               ///< Запись закончена штатно (нет — после аварийного завершения).

    std::vector<Blob> blobs;            ///< Блобы, на которые ссылаются команды (по хешу, без повторов).

    /// @brief Добавляет команду тика. Тики — по неубыванию.
    void add(std::uint32_t tick, const Command& command);
    /// @brief Добавляет блоб (повтор по хешу игнорируется); возвращает хеш.
    std::uint64_t add_blob(std::span<const std::byte> bytes);
    [[nodiscard]] const Blob* find_blob(std::uint64_t hash) const noexcept;
    /// @brief Команды тика (пустой span, если их не было).
    [[nodiscard]] std::span<const Command> at(std::uint32_t tick) const noexcept;
    [[nodiscard]] std::size_t command_count() const noexcept { return m_commands.size(); }
    /// @brief Все команды по порядку записи и тик каждой.
    [[nodiscard]] std::span<const Command> commands() const noexcept { return m_commands; }
    [[nodiscard]] std::span<const std::uint32_t> command_ticks() const noexcept { return m_ticks; }

    /// @brief Сохраняет файл-журнал целиком (с избыточностью). Для записи по ходу игры используйте `Session::record`.
    [[nodiscard]] std::expected<void, std::string> save(const std::string& path) const;
    /// @brief Читает файл любого формата; журнал при этом восстанавливается по чётности, файл не меняется.
    [[nodiscard]] static std::expected<Recording, std::string> load(const std::string& path);
    [[nodiscard]] static std::expected<std::pair<Recording, LoadInfo>, std::string> load_with_info(const std::string& path);
    /// @brief Исправляет повреждения журнала в файле на месте (переписывает восстановленные блоки).
    [[nodiscard]] static std::expected<EventLog::RecoveryReport, std::string> repair_file(const std::string& path);

    [[nodiscard]] friend bool operator==(const Recording&, const Recording&) noexcept = default;

private:
    std::vector<std::uint32_t> m_ticks;
    std::vector<Command> m_commands;
};

/// @brief Итог сессии: для повтора — совпали ли хеши с записанными.
struct Verdict {
    bool checked = false;  ///< Повтор: хеши сравнивались (у оборванной записи сравнивать не с чем).
    bool match = true;     ///< Хеши совпали (или сравнивать не с чем).
    bool complete = true;  ///< Повторяемая запись была закончена штатно.
    StateHashes expected{};
    StateHashes actual{};
    std::vector<std::string> differing; ///< Подсистемы, у которых хеши разошлись.
};

/// @brief Режим прогона: без записи, запись или повтор.
class Session {
public:
    enum class Mode : std::uint8_t { Off, Record, Replay };

    /// @brief Разбирает `--record <файл>`, `--replay <файл>`, `--seed N`. Ошибка — файл не открылся или не прочитан.
    [[nodiscard]] static std::expected<Session, std::string> from_args(std::span<const std::string> args, std::uint64_t default_seed,
                                                                       const CommandRegistry* registry = nullptr);
    [[nodiscard]] static Session off(std::uint64_t seed) { return Session(Mode::Off, seed); }
    /**
     * @brief Запись. С `path` команды пишутся в файл-журнал **по ходу игры** и сбрасываются на диск каждые 60 тиков:
     * при аварийном завершении теряется не больше последней секунды. Без `path` — только в памяти (`recording()`).
     * Схемы из `registry` попадут в файл. Ошибка — файл не создаётся.
     */
    [[nodiscard]] static std::expected<Session, std::string> record(std::uint64_t seed, std::string path = {}, const CommandRegistry* registry = nullptr);
    [[nodiscard]] static Session replay(Recording recording);

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return m_recording.seed; }
    /// @brief Повтор: все записанные тики исполнены.
    [[nodiscard]] bool finished(std::uint32_t tick) const noexcept { return m_mode == Mode::Replay && tick >= m_recording.tick_count; }
    [[nodiscard]] std::uint32_t recorded_ticks() const noexcept { return m_recording.tick_count; }

    /**
     * @brief Кладёт блоб в запись (режим Record; в повторе блобы уже в записи) и возвращает его хеш.
     * Вызывать **до** тика, где его использует команда: в файл блоб попадает раньше команды.
     */
    std::uint64_t add_blob(std::uint32_t tick, std::span<const std::byte> bytes);
    /// @brief Команды, которые нужно применить на этом тике: при повторе — записанные, иначе `live` (и запись).
    [[nodiscard]] std::span<const Command> begin_tick(std::uint32_t tick, std::span<const Command> live);
    /// @brief Заканчивает прогон: запись дописывает трейлер (хеши) и сбрасывает файл, повтор сверяет хеши по подсистемам.
    [[nodiscard]] std::expected<Verdict, std::string> finish(std::uint32_t ticks_run, const StateHashes& hashes);

    [[nodiscard]] const Recording& recording() const noexcept { return m_recording; }

private:
    Session(Mode mode, std::uint64_t seed);

    Mode m_mode;
    Recording m_recording;
    std::unique_ptr<EventLog::FileStorage> m_storage; ///< Файл записи (только режим Record с путём).
    std::unique_ptr<EventLog::Writer> m_writer;
    std::uint32_t m_last_flush = 0; ///< Тик последнего сброса записи на диск.
};

/// @brief Первое расхождение двух записей.
struct Difference {
    enum class Kind : std::uint8_t { Seed, TickCount, Command, CommandCount, FinalHash };
    Kind kind = Kind::Seed;
    std::uint32_t tick = 0; ///< Для Command: тик расхождения.
    std::string text;       ///< Человекочитаемо; для хешей — какие именно подсистемы разошлись.
};

/// @brief Текст записи целиком: заголовок, схемы команд, команды по тикам. Не требует знания игры.
[[nodiscard]] std::string inspect(const Recording& recording, std::size_t max_commands = static_cast<std::size_t>(-1));
/// @brief Текст о том, как прочитан файл: формат, восстановленные блоки, пропуски, оборванная запись.
[[nodiscard]] std::string describe(const LoadInfo& info);
/// @brief Первое расхождение (сид, команды по порядку, число тиков, хеши) или `nullopt`, если записи совпадают по смыслу.
[[nodiscard]] std::optional<Difference> diff(const Recording& a, const Recording& b);

/// @brief Текст итога для консоли: «recorded N ticks…» / «replay: hashes MATCH|DIFFER FROM… (расходятся: mana)».
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
    /// @brief Текстовый дамп (тик, хеши по подсистемам, команды); `false` — файл не открылся.
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
/// Симуляция, принимающая блобы по ссылкам команд (необязательно): `Driver` отдаёт ей блобы записи при повторе.
template<typename Sim>
concept BlobSink = requires(Sim& s, std::span<const std::byte> bytes) { s.provide_blob(bytes); };

template<Simulatable Sim>
class Driver {
public:
    /// Повтор: блобы записи сразу отдаются симуляции (если она умеет их принимать, `provide_blob`) — ссылки команд на них
    /// (хеш программы) к моменту команды уже разрешимы.
    Driver(Sim& sim, Session& session, FlightRecorder* recorder = nullptr) : m_sim(&sim), m_session(&session), m_recorder(recorder) {
        if constexpr (BlobSink<Sim>) {
            if (m_session->mode() == Session::Mode::Replay) {
                for (const Blob& blob : m_session->recording().blobs) m_sim->provide_blob(blob.bytes);
            }
        }
    }

    /**
     * @brief Блоб на этом тике (программа заклинания и т. п.): в запись и в симуляцию; возвращает хеш для команды.
     * Вызывать до `step`, в котором уйдёт команда со ссылкой. При повторе ничего не делает (блобы уже в записи).
     */
    std::uint64_t submit_blob(std::span<const std::byte> bytes) {
        const std::uint64_t hash = m_session->add_blob(m_sim->tick_number(), bytes);
        if constexpr (BlobSink<Sim>) {
            if (m_session->mode() != Session::Mode::Replay) m_sim->provide_blob(bytes);
        }
        return hash;
    }

    /// @brief Один тик симуляции с командами сессии. `false` — повтор дошёл до конца записи (тик не выполнялся).
    bool step(std::span<const Command> live) {
        const std::uint32_t tick = m_sim->tick_number();
        if (m_session->finished(tick)) return false;
        const std::span<const Command> commands = m_session->begin_tick(tick, live);
        m_sim->tick(commands);
        if (m_recorder != nullptr) m_recorder->push(tick, commands, std::as_const(*m_sim).hashes());
        return true;
    }
    /// @brief Закрывает сессию: запись пишет трейлер с хешами, повтор сверяет хеши с записанными.
    [[nodiscard]] std::expected<Verdict, std::string> finish() { return m_session->finish(m_sim->tick_number(), std::as_const(*m_sim).hashes()); }
    /// @brief Повтор: команды берутся из записи — ввод читать не нужно.
    [[nodiscard]] bool replaying() const noexcept { return m_session->mode() == Session::Mode::Replay; }

private:
    Sim* m_sim;
    Session* m_session;
    FlightRecorder* m_recorder;
};

} // namespace Replay
