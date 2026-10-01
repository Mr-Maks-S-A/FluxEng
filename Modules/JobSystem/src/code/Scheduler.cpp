#include <JobSystem/Scheduler.hpp>

#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace JobSystem {

namespace {

// Какой планировщик и какой номер у текущего потока: нужно для scratch() и this_thread_index().
thread_local const Scheduler* t_owner = nullptr;
thread_local unsigned t_index = 0;

// Сколько раз рабочий поток проверяет очередь перед тем, как уснуть (~десятки микросекунд).
constexpr int k_spin_before_sleep = 4096;

// Подсказка процессору «я жду в цикле»: экономит энергию и не мешает соседнему гиперпотоку.
inline void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

} // namespace

unsigned default_threads() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? hardware - 1 : 0;
}

Scheduler::Scheduler(SchedulerConfig config) {
    m_scratch.reserve(config.threads + 1);
    for (unsigned i = 0; i <= config.threads; ++i) {
        m_scratch.push_back(MemorySystem::Arena::reserve(config.scratch_bytes));
    }
    m_threads.reserve(config.threads);
    for (unsigned i = 1; i <= config.threads; ++i) {
        m_threads.emplace_back([this, i] { worker_loop(i); });
    }
}

Scheduler::~Scheduler() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads) thread.join();
}

void Scheduler::run(JobCounter& counter, Job job) {
    counter.m_pending.fetch_add(1, std::memory_order_relaxed);
    Task task{std::move(job), &counter};
    if (m_threads.empty()) {
        execute(task); // нет фоновых потоков — выполняем сразу
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(task));
        m_queued.fetch_add(1, std::memory_order_relaxed);
    }
    // Поток, который собрался спать, увеличивает m_sleeping под мьютексом и перепроверяет очередь.
    // Значит, либо он увидит нашу задачу, либо мы увидим его в m_sleeping — пропустить пробуждение нельзя.
    if (m_sleeping.load(std::memory_order_seq_cst) > 0) m_wake.notify_one();
}

void Scheduler::wait(JobCounter& counter) {
    int idle = 0;
    while (!counter.done()) {
        Task task;
        if (try_pop(task)) {
            m_helped.fetch_add(1, std::memory_order_relaxed);
            execute(task); // помогаем, а не спим: так вложенные задачи не блокируют друг друга
            idle = 0;
        } else if (++idle < 64) {
            cpu_relax(); // наши задачи уже выполняются другими потоками — обычно это микросекунды
        } else {
            std::this_thread::yield();
        }
    }
    if (std::exception_ptr error = counter.take_error()) {
        std::rethrow_exception(error);
    }
}

MemorySystem::Arena& Scheduler::scratch() noexcept { return m_scratch[this_thread_index()]; }

unsigned Scheduler::this_thread_index() const noexcept { return t_owner == this ? t_index : 0u; }

void Scheduler::worker_loop(unsigned index) {
    t_owner = this;
    t_index = index;
    for (;;) {
        Task task;
        if (try_pop(task)) {
            execute(task);
            continue;
        }
        // Работы нет: сначала недолго крутимся — следующая система тика скорее всего вот-вот даст задачи.
        bool found = false;
        for (int spin = 0; spin < k_spin_before_sleep && !found; ++spin) {
            cpu_relax();
            found = m_queued.load(std::memory_order_relaxed) > 0;
        }
        if (found) continue;

        std::unique_lock lock(m_mutex);
        m_sleeping.fetch_add(1, std::memory_order_seq_cst);
        m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
        m_sleeping.fetch_sub(1, std::memory_order_relaxed);
        if (m_queue.empty()) return; // m_stopping и очередь пуста
        task = std::move(m_queue.front());
        m_queue.pop_front();
        m_queued.fetch_sub(1, std::memory_order_relaxed);
        lock.unlock();
        execute(task);
    }
}

bool Scheduler::try_pop(Task& out) {
    if (m_queued.load(std::memory_order_relaxed) == 0) return false; // без мьютекса: очередь пуста
    std::lock_guard lock(m_mutex);
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    m_queued.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void Scheduler::execute(Task& task) noexcept {
    try {
        task.job();
    } catch (...) {
        task.counter->capture(std::current_exception());
    }
    // Задачу уничтожаем ДО обнуления счётчика: после него ждущий поток может выйти из wait()
    // и снять со стека то, на что ссылаются захваты задачи.
    task.job = nullptr;
    m_executed.fetch_add(1, std::memory_order_relaxed);
    // release: всё, что задача записала, видно тому, кто увидит обнуление счётчика (acquire в done()).
    task.counter->m_pending.fetch_sub(1, std::memory_order_release);
}

} // namespace JobSystem
