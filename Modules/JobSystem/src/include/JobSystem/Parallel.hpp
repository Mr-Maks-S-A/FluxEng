#pragma once
/**
 * @file Parallel.hpp
 * @brief Параллельные алгоритмы поверх Scheduler: parallel_for, parallel_reduce, ChunkBuffers.
 *
 * **Нарезка детерминирована.** Диапазон [0, count) режется на куски по `grain` элементов:
 * кусок i — это [i·grain, min((i+1)·grain, count)). Границы зависят только от count и grain,
 * а не от числа потоков и не от того, кто какой кусок взял. Поэтому, если каждый кусок пишет
 * только свои данные, а частичные результаты сливаются **по номеру куска** (parallel_reduce,
 * ChunkBuffers), итог одинаков при 0, 1 и 16 потоках — и совпадает с последовательным.
 *
 * Куски раздаются через атомарный счётчик: никакой очереди на каждый кусок, рабочий поток
 * берёт следующий номер одним fetch_add. Вызывающий поток тоже берёт куски.
 *
 * @code
 * JobSystem::parallel_for(jobs, bodies.size(), 1024, [&](std::size_t begin, std::size_t end) {
 *     for (std::size_t i = begin; i < end; ++i) bodies[i].position += bodies[i].velocity * dt;
 * });
 * @endcode
 */

#include <JobSystem/Scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>
#include <vector>

namespace JobSystem {

/// @brief Сколько кусков получится из `count` элементов по `grain` (grain = 0 считается как 1).
[[nodiscard]] constexpr std::size_t chunk_count(std::size_t count, std::size_t grain) noexcept {
    grain = grain == 0 ? 1 : grain;
    return (count + grain - 1) / grain;
}

namespace detail {

template<typename Fn>
void call_chunk(Fn& fn, std::size_t begin, std::size_t end, std::size_t chunk) {
    if constexpr (std::is_invocable_v<Fn&, std::size_t, std::size_t, std::size_t>) {
        fn(begin, end, chunk);
    } else {
        static_assert(std::is_invocable_v<Fn&, std::size_t, std::size_t>,
                      "parallel_for: body must accept (begin, end) or (begin, end, chunk)");
        fn(begin, end);
    }
}

} // namespace detail

/**
 * @brief Выполняет `body` для всех кусков диапазона [0, count) параллельно и ждёт завершения.
 *
 * @param scheduler Планировщик.
 * @param count     Число элементов.
 * @param grain     Элементов в куске. Кусок должен быть «весомым» (десятки микросекунд работы),
 *                  иначе накладные расходы съедят выигрыш.
 * @param body      `body(begin, end)` или `body(begin, end, chunk)`.
 * @throws Первое исключение из `body` (остальные куски при этом всё равно выполняются).
 */
template<typename Fn>
void parallel_for(Scheduler& scheduler, std::size_t count, std::size_t grain, Fn&& body) {
    grain = grain == 0 ? 1 : grain;
    const std::size_t chunks = chunk_count(count, grain);
    if (chunks == 0) return;
    if (chunks == 1 || scheduler.threads() == 0) {
        for (std::size_t c = 0; c < chunks; ++c) detail::call_chunk(body, c * grain, std::min(count, (c + 1) * grain), c);
        return;
    }

    std::atomic<std::size_t> next{0};
    auto drain = [&] {
        for (std::size_t c = next.fetch_add(1, std::memory_order_relaxed); c < chunks;
             c = next.fetch_add(1, std::memory_order_relaxed)) {
            detail::call_chunk(body, c * grain, std::min(count, (c + 1) * grain), c);
        }
    };

    // Помощников не больше, чем есть потоков и лишних кусков. Каждый крутит drain, пока куски не кончатся.
    JobCounter helpers;
    const std::size_t helper_count = std::min<std::size_t>(scheduler.threads(), chunks - 1);
    for (std::size_t h = 0; h < helper_count; ++h) scheduler.run(helpers, drain);

    std::exception_ptr own_error;
    try {
        drain(); // вызывающий поток тоже работает
    } catch (...) {
        own_error = std::current_exception();
        next.store(chunks, std::memory_order_relaxed); // остальные куски никто уже не возьмёт… кроме уже начатых
    }
    scheduler.wait(helpers); // стек с next и body живёт, пока помощники не закончили
    if (own_error) std::rethrow_exception(own_error);
}

/**
 * @brief Параллельная свёртка с детерминированным порядком слияния.
 *
 * `map(begin, end)` считает частичный результат куска; частичные результаты сливаются
 * `combine(acc, part)` **по возрастанию номера куска** — итог не зависит от числа потоков,
 * даже для чисел с плавающей точкой.
 */
template<typename T, typename Map, typename Combine>
[[nodiscard]] T parallel_reduce(Scheduler& scheduler, std::size_t count, std::size_t grain, T init, Map&& map,
                                Combine&& combine) {
    const std::size_t chunks = chunk_count(count, grain);
    std::vector<T> partial(chunks, init);
    parallel_for(scheduler, count, grain,
                 [&](std::size_t begin, std::size_t end, std::size_t chunk) { partial[chunk] = map(begin, end); });
    T result = std::move(init);
    for (T& part : partial) result = combine(std::move(result), std::move(part));
    return result;
}

/**
 * @brief Выходные буферы по одному на кусок: параллельная запись без блокировок, слияние по порядку.
 *
 * Типичный случай — параллельная система порождает события (попадания, столкновения):
 * каждый кусок пишет в свой буфер, а потом один поток отправляет их в шину в порядке кусков.
 * Результат детерминирован при любом числе потоков. Ёмкость буферов сохраняется между тиками:
 * в устойчивом режиме аллокаций нет.
 */
template<typename T>
class ChunkBuffers {
public:
    /// @brief Готовит `chunks` пустых буферов (ёмкость прошлых сохраняется).
    void reset(std::size_t chunks) {
        if (m_buffers.size() < chunks) m_buffers.resize(chunks);
        for (std::size_t c = 0; c < std::max(m_used, chunks); ++c) m_buffers[c].items.clear();
        m_used = chunks;
    }

    /// @brief Буфер куска `chunk` (пишет только поток, выполняющий этот кусок).
    [[nodiscard]] std::vector<T>& operator[](std::size_t chunk) noexcept { return m_buffers[chunk].items; }

    /// @brief Обходит все элементы в порядке кусков, затем в порядке записи.
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t c = 0; c < m_used; ++c) {
            for (const T& item : m_buffers[c].items) fn(item);
        }
    }

    /// @brief Всего элементов во всех буферах.
    [[nodiscard]] std::size_t total() const noexcept {
        std::size_t n = 0;
        for (std::size_t c = 0; c < m_used; ++c) n += m_buffers[c].items.size();
        return n;
    }

    /// @brief Число используемых буферов (кусков).
    [[nodiscard]] std::size_t chunks() const noexcept { return m_used; }

private:
    // Каждый буфер — в своей кэш-линии: заголовки соседних std::vector (begin/end/capacity),
    // в которые одновременно пишут разные потоки, иначе делили бы одну линию (false sharing).
    struct alignas(64) Slot {
        std::vector<T> items;
    };
    std::vector<Slot> m_buffers;
    std::size_t m_used = 0;
};

} // namespace JobSystem
