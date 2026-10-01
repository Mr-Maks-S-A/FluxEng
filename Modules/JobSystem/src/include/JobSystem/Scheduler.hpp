#pragma once
/**
 * @file Scheduler.hpp
 * @brief Планировщик задач: пул рабочих потоков, счётчики, ожидание с помощью.
 *
 * Модель:
 * - задача (Job) — функция без аргументов; запускается через run() и привязана к счётчику (JobCounter);
 * - wait(counter) ждёт, пока счётчик не обнулится, и **сам выполняет задачи** из очереди, а не спит.
 *   Поэтому задача может запускать подзадачи и ждать их (вложенный parallel_for) без взаимной блокировки;
 * - у каждого потока своя временная арена (scratch()): выделение — сдвиг указателя, без блокировок.
 *
 * Потоков `threads` — это **фоновые** потоки. Поток, который ждёт, тоже работает,
 * поэтому всего исполнителей concurrency() = threads + 1. При `threads = 0` всё выполняется
 * прямо в вызывающем потоке — удобно для отладки и для сравнения «1 поток против N».
 *
 * @code
 * JobSystem::Scheduler jobs;                       // потоков: число ядер − 1
 * JobSystem::JobCounter done;
 * jobs.run(done, [] { load_textures(); });
 * jobs.run(done, [] { build_navmesh(); });
 * jobs.wait(done);                                 // ждущий поток помогает выполнять задачи
 * @endcode
 */

#include <MemorySystem/Arena.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace JobSystem {

/// @brief Число фоновых потоков по умолчанию: ядра − 1 (одно ядро — потоку, который ждёт).
[[nodiscard]] unsigned default_threads() noexcept;

/// @brief Параметры планировщика.
struct SchedulerConfig {
    unsigned threads = default_threads();                    ///< Фоновых потоков; 0 — всё в вызывающем потоке.
    std::size_t scratch_bytes = MemorySystem::MiB(64);       ///< Резерв временной арены каждого потока.
};

/**
 * @brief Счётчик незавершённых задач. Ноль — «всё выполнено».
 *
 * **ZII.** `JobCounter{}` — «ничего не запущено»: wait() на нём возвращается сразу.
 * Хранит первое исключение из своих задач; wait() перебрасывает его.
 */
class JobCounter {
public:
    JobCounter() noexcept = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    /// @brief Задач, которые ещё не завершились.
    [[nodiscard]] int pending() const noexcept { return m_pending.load(std::memory_order_acquire); }
    /// @brief `true`, если все задачи завершены.
    [[nodiscard]] bool done() const noexcept { return pending() == 0; }

private:
    friend class Scheduler;

    void capture(std::exception_ptr error) noexcept {
        std::lock_guard lock(m_error_mutex);
        if (!m_error) m_error = std::move(error);
    }
    std::exception_ptr take_error() noexcept {
        std::lock_guard lock(m_error_mutex);
        return std::exchange(m_error, nullptr);
    }

    std::atomic<int> m_pending{0};
    std::mutex m_error_mutex;
    std::exception_ptr m_error;
};

/// @brief Счётчики планировщика (для отладки и тестов).
struct SchedulerStats {
    std::uint64_t jobs_executed = 0;  ///< Всего выполнено задач.
    std::uint64_t helped_while_waiting = 0; ///< Из них выполнено потоком внутри wait().
};

/**
 * @brief Пул потоков с общей очередью задач.
 *
 * Не копируется и не перемещается: задачи держат ссылку на планировщик.
 * Деструктор дожидается завершения рабочих потоков (очередь к этому моменту должна быть пуста).
 *
 * Оставшись без работы, рабочий поток несколько десятков микросекунд крутится в ожидании
 * (инструкция pause) и только потом засыпает: задачи следующей системы того же тика
 * подхватываются без системного вызова на пробуждение.
 */
class Scheduler {
public:
    /// @brief Тип задачи.
    using Job = std::function<void()>;

    /// @brief Запускает `config.threads` фоновых потоков.
    explicit Scheduler(SchedulerConfig config = {});
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    ~Scheduler();

    /// @brief Фоновых потоков.
    [[nodiscard]] unsigned threads() const noexcept { return static_cast<unsigned>(m_threads.size()); }
    /// @brief Всего исполнителей: фоновые потоки + поток, который ждёт.
    [[nodiscard]] unsigned concurrency() const noexcept { return threads() + 1; }

    /**
     * @brief Ставит задачу в очередь и увеличивает счётчик.
     *
     * Без фоновых потоков задача выполняется сразу, в этом же вызове.
     * Исключение из задачи сохраняется в счётчике и перебрасывается из wait().
     */
    void run(JobCounter& counter, Job job);

    /**
     * @brief Ждёт обнуления счётчика, выполняя задачи из очереди.
     * @throws Первое исключение, выброшенное задачами этого счётчика.
     */
    void wait(JobCounter& counter);

    /**
     * @brief Временная арена текущего потока.
     *
     * Каждый поток пишет только в свою арену — без блокировок. Освобождайте через ArenaScope.
     * Для потока, который не принадлежит планировщику, возвращается арена «вызывающего» потока:
     * пользоваться ею можно только из одного такого потока.
     */
    [[nodiscard]] MemorySystem::Arena& scratch() noexcept;

    /// @brief Номер текущего потока: 0 — вызывающий (не рабочий), 1…threads() — рабочие.
    [[nodiscard]] unsigned this_thread_index() const noexcept;

    /// @brief Счётчики выполненных задач.
    [[nodiscard]] SchedulerStats stats() const noexcept {
        return {m_executed.load(std::memory_order_relaxed), m_helped.load(std::memory_order_relaxed)};
    }

private:
    struct Task {
        Job job;
        JobCounter* counter = nullptr;
    };

    void worker_loop(unsigned index);
    bool try_pop(Task& out);
    void execute(Task& task) noexcept;

    std::vector<std::thread> m_threads;
    std::vector<MemorySystem::Arena> m_scratch; ///< [0] — вызывающий поток, [i] — рабочий i.

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Task> m_queue;
    bool m_stopping = false;

    // Чтобы не платить за системные вызовы на каждой задаче:
    // m_queued — сколько задач в очереди (проверка без мьютекса),
    // m_sleeping — сколько потоков спит на m_wake (будим, только если кто-то спит).
    std::atomic<std::size_t> m_queued{0};
    std::atomic<unsigned> m_sleeping{0};

    std::atomic<std::uint64_t> m_executed{0};
    std::atomic<std::uint64_t> m_helped{0};
};

/**
 * @brief Группа задач с собственным счётчиком; деструктор дожидается всех.
 *
 * @code
 * JobSystem::TaskGroup group(jobs);
 * group.run([&] { physics.step(); });
 * group.run([&] { audio.mix(); });
 * group.wait();
 * @endcode
 */
class TaskGroup {
public:
    /// @brief Группа задач планировщика `scheduler`.
    explicit TaskGroup(Scheduler& scheduler) noexcept : m_scheduler(scheduler) {}
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    /// @brief Дожидается задач; исключения при этом проглатываются — вызывайте wait() явно.
    ~TaskGroup() {
        try {
            m_scheduler.wait(m_counter);
        } catch (...) {
        }
    }

    /// @brief Запускает задачу в группе.
    void run(Scheduler::Job job) { m_scheduler.run(m_counter, std::move(job)); }
    /// @brief Ждёт все задачи группы; перебрасывает первое исключение.
    void wait() { m_scheduler.wait(m_counter); }

private:
    Scheduler& m_scheduler;
    JobCounter m_counter;
};

} // namespace JobSystem
