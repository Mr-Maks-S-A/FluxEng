#pragma once
/**
 * @file Runtime.hpp
 * @brief Ядро игры без графики и окна: шина событий, задачи, память тика и кадра, фиксированный тик, модули.
 *
 * Runtime — то, что нужно и клиенту, и выделенному серверу. Клиент (Core::App) добавляет к нему окно, рендер и ввод;
 * сервер использует его как есть.
 *
 * @code
 * struct Physics final : RuntimeSystem::Module {
 *     std::string_view name() const noexcept override { return "Physics"; }
 *     void declare(RuntimeSystem::Runtime& rt) override { rt.bus().declare_module("Physics").produces<Hit>(); }
 *     void tick(RuntimeSystem::Runtime& rt) override { … }
 * };
 *
 * RuntimeSystem::Runtime runtime({.ticks_per_second = 60.0});
 * runtime.add<Physics>();
 * runtime.add<Combat>();          // Combat::Combat() вызывает depends_on("Physics")
 * runtime.initialize();           // порядок по зависимостям
 * runtime.run({.max_ticks = 600}); // сервер: реальное время, пока не request_stop() или лимит тиков
 * // ~Runtime() или runtime.shutdown() — shutdown() модулей в обратном порядке
 * @endcode
 *
 * Тик: модули по порядку → обратный вызов игры (set_tick_callback) → EventBus::advance_tick() → смена арен.
 * Кадр: begin_frame() (сброс арены кадра, Module::frame) → update() (advance_frame, нужное число тиков).
 *
 * Потоки: Runtime и его модули вызываются из одного потока; параллелизм — через jobs().
 * Исключения: add/initialize бросают RuntimeError (контракт нарушен) или то, что бросил модуль; tick() пробрасывает
 * исключение модуля — состояние остаётся Running, а shutdown() всё равно произойдёт в деструкторе.
 */

#include <RuntimeSystem/FixedStep.hpp>
#include <RuntimeSystem/Module.hpp>

#include <EventSystem/EventSystem.hpp>
#include <JobSystem/JobSystem.hpp>
#include <MemorySystem/MemorySystem.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RuntimeSystem {

/// @brief Нарушение контракта: дубликат имени, неизвестная зависимость, цикл, вызов не в той фазе.
class RuntimeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Фаза жизни Runtime. Только вперёд: Created → Initializing → Running → ShuttingDown → Stopped.
enum class Phase : std::uint8_t {
    Created,      ///< Можно add(); модули ещё не тронуты.
    Initializing, ///< Идёт declare/init.
    Running,      ///< Можно tick()/update()/run().
    ShuttingDown, ///< Идёт shutdown() модулей.
    Stopped,      ///< Всё закончено (или initialize() не удался и откатился). Повторно использовать нельзя.
};

/// @brief Параметры Runtime.
struct RuntimeConfig {
    double ticks_per_second = 30.0;   ///< Частота симуляции при скорости x1.
    int max_ticks_per_frame = 16;     ///< Защита от «спирали смерти».
    bool lockstep = false;            ///< Один тик на update() без учёта времени (детерминированные прогоны).
    int threads = -1;                 ///< Фоновых потоков JobSystem: -1 — ядра − 1, 0 — всё в вызывающем потоке.
    std::size_t tick_arena_bytes = MemorySystem::MiB(256); ///< Резерв каждой из двух арен тика (виртуальный).
    std::size_t frame_arena_bytes = MemorySystem::MiB(64); ///< Резерв арены кадра (виртуальный).
    bool profile_modules = false;     ///< Мерить время tick() каждого модуля (ModuleStats): два чтения часов на модуль.
};

/// @brief Время работы модуля в tick(), если включено RuntimeConfig::profile_modules.
struct ModuleStats {
    std::string_view name;       ///< Имя модуля.
    std::uint64_t ticks = 0;     ///< Сколько раз вызван tick().
    std::uint64_t total_ns = 0;  ///< Суммарное время.
    std::uint64_t max_ns = 0;    ///< Самый долгий тик.
};

/// @brief Параметры Runtime::run().
struct RunOptions {
    int max_ticks = -1;     ///< Выйти после N тиков; -1 — пока не request_stop().
    bool realtime = true;   ///< true — темп по часам (сервер); false — тики подряд без ожидания (тесты, симуляция вперёд).
};

class Runtime {
public:
    explicit Runtime(RuntimeConfig config = {});
    /// @brief Завершает модули (если ещё не завершены) и уничтожает их в обратном порядке.
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete; // модули хранят ссылки на Runtime
    Runtime& operator=(Runtime&&) = delete;

    // ================================================================= модули

    /// @brief Создаёт модуль `M` и регистрирует его. Только в фазе Created.
    /// @return Ссылка на модуль; живёт до уничтожения Runtime.
    /// @throws RuntimeError Не в фазе Created.
    template<std::derived_from<Module> M, typename... Args>
    M& add(Args&&... args) {
        require_phase(Phase::Created, "add");
        auto module = std::make_unique<M>(std::forward<Args>(args)...);
        M& ref = *module;
        register_module(std::move(module));
        return ref;
    }

    /// @brief Модуль по имени или nullptr.
    [[nodiscard]] Module* find(std::string_view name) noexcept;
    [[nodiscard]] const Module* find(std::string_view name) const noexcept;

    /// @brief Модуль типа `M` или nullptr (первый подходящий).
    template<std::derived_from<Module> M>
    [[nodiscard]] M* find() noexcept {
        for (const auto& slot : m_modules) {
            if (auto* typed = dynamic_cast<M*>(slot.module.get())) return typed;
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t module_count() const noexcept { return m_modules.size(); }

    // ================================================================= жизненный цикл

    /// @brief Упорядочить модули по зависимостям, вызвать declare() всех, затем init() по порядку.
    /// При исключении уже инициализированные модули завершаются (в обратном порядке), фаза становится Stopped,
    /// исключение летит дальше. Ошибка зависимостей (RuntimeError) бросается до первого хука и фазу не меняет.
    /// @throws RuntimeError Дубликат имени, неизвестная зависимость, цикл или не та фаза.
    void initialize();

    /// @brief Завершает модули в обратном порядке init. Идемпотентна; безопасна из деструктора.
    void shutdown() noexcept;

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool running() const noexcept { return m_phase == Phase::Running; }

    /// @brief Имена модулей в порядке инициализации (после initialize()).
    [[nodiscard]] std::vector<std::string_view> initialization_order() const;

    // ================================================================= время

    /// @brief Начало кадра: сбросить арену кадра, вызвать Module::frame у всех.
    void begin_frame(double frame_seconds);

    /// @brief Продвинуть шину кадра и выполнить столько тиков, сколько набежало. @return Число тиков.
    int update(double frame_seconds);

    /// @brief Ровно один тик (минуя FixedStep): модули → обратный вызов игры → advance_tick → смена арен.
    void tick();

    /// @brief Обратный вызов игры: вызывается в каждом тике после модулей, до advance_tick().
    void set_tick_callback(std::function<void(Runtime&)> callback) { m_tick_callback = std::move(callback); }

    /// @brief Цикл без окна (выделенный сервер, тесты). Если модули ещё не инициализированы — initialize().
    /// @return Сколько тиков выполнено.
    int run(RunOptions options = {});

    /// @brief Попросить run() выйти после текущего тика. Можно из другого потока и из обработчика сигнала.
    void request_stop() noexcept { m_stop.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool stop_requested() const noexcept { return m_stop.load(std::memory_order_relaxed); }

    // ================================================================= сервисы

    [[nodiscard]] EventSystem::EventBus& bus() noexcept { return m_bus; }
    [[nodiscard]] const EventSystem::EventBus& bus() const noexcept { return m_bus; }
    [[nodiscard]] JobSystem::Scheduler& jobs() noexcept { return m_jobs; }
    [[nodiscard]] FixedStep& step() noexcept { return m_step; }
    [[nodiscard]] const FixedStep& step() const noexcept { return m_step; }
    [[nodiscard]] const RuntimeConfig& config() const noexcept { return m_config; }

    /// @brief Номер текущего тика.
    [[nodiscard]] EventSystem::Tick current_tick() const noexcept { return m_bus.current_tick(); }

    /// @brief Память текущего тика: читается в N+1 через previous_tick_arena(), затем обнуляется.
    [[nodiscard]] MemorySystem::Arena& tick_arena() noexcept { return m_tick_memory.current(); }
    [[nodiscard]] const MemorySystem::Arena& previous_tick_arena() const noexcept { return m_tick_memory.previous(); }
    /// @brief Временная память кадра: очищается в begin_frame().
    [[nodiscard]] MemorySystem::Arena& frame_arena() noexcept { return m_frame_memory; }

    /// @brief Время tick() модулей (пусто, если profile_modules выключен).
    [[nodiscard]] std::vector<ModuleStats> module_stats() const;

private:
    struct Slot {
        std::unique_ptr<Module> module;
        ModuleStats stats{};
    };

    void register_module(std::unique_ptr<Module> module);
    void require_phase(Phase expected, std::string_view action) const;
    void sort_modules();
    void rollback(std::size_t initialized) noexcept;
    void tick_modules();

    RuntimeConfig m_config;
    EventSystem::EventBus m_bus;
    JobSystem::Scheduler m_jobs;
    MemorySystem::DoubleArena m_tick_memory;
    MemorySystem::Arena m_frame_memory;
    FixedStep m_step;
    std::function<void(Runtime&)> m_tick_callback;
    std::atomic<bool> m_stop{false};
    Phase m_phase = Phase::Created;
    std::size_t m_initialized = 0;
    // Последним: модули уничтожаются раньше шины, задач и арен, которыми пользуются.
    std::vector<Slot> m_modules;
};

} // namespace RuntimeSystem
