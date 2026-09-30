#pragma once
/**
 * @file Arena.hpp
 * @brief Линейная арена (bump allocator) с инвариантом ZII: свободная часть всегда нулевая.
 *
 * Арена выдаёт память, сдвигая указатель вперёд, и освобождает её только целиком
 * или «откатом» к ранее запомненной отметке. Отдельных `free` нет — поэтому выделение
 * стоит несколько инструкций, а утечки невозможны по построению.
 *
 * **Инвариант ZII.** Байты от текущей позиции до конца подтверждённой памяти всегда равны нулю.
 * - Выделение (push) ничего не зануляет — память уже нулевая: это просто сдвиг указателя.
 * - Откат (pop_to, reset) зануляет освобождённый участок — платим один раз и только за то,
 *   что реально использовали.
 * - Новые страницы виртуальной арены ОС отдаёт нулевыми.
 *
 * @code
 * MemorySystem::Arena arena = MemorySystem::Arena::reserve(MemorySystem::MiB(64));
 * Particle* p = arena.push<Particle>();                       // все поля = 0
 * std::span<Vec2> points = arena.push_array<Vec2>(1024);      // 1024 нулевых точки
 * {
 *     MemorySystem::ArenaScope scratch(arena);                // временная память
 *     auto tmp = arena.push_array<int>(4096);
 * }                                                           // откат и зануление здесь
 * @endcode
 *
 * @note Арена не потокобезопасна: одна арена — один поток (или внешняя синхронизация).
 */

#include <MemorySystem/Core.hpp>
#include <MemorySystem/VirtualMemory.hpp>

#include <cassert>
#include <cstddef>
#include <span>

namespace MemorySystem {

/**
 * @brief Отметка позиции арены для отката.
 *
 * Нулевая отметка (`ArenaMarker{}`) — начало арены: откат к ней равен reset().
 */
struct ArenaMarker {
    std::size_t position = 0; ///< Смещение от начала арены, байт.
};

/// @brief Счётчики арены.
struct ArenaStats {
    std::size_t used = 0;      ///< Занято сейчас, байт (включая выравнивание).
    std::size_t peak = 0;      ///< Максимум `used` за всё время.
    std::size_t committed = 0; ///< Подтверждено физической памяти, байт.
    std::size_t capacity = 0;  ///< Предел роста, байт.
};

/**
 * @brief Линейная арена с нулевой свободной памятью.
 *
 * Два источника памяти:
 * - reserve(): виртуальный резерв, физическая память подтверждается шагами по мере роста;
 * - over(): внешний буфер (например, на стеке или внутри другой арены).
 *
 * Созданная по умолчанию арена пуста и валидна (ZII): любой push возвращает `nullptr`.
 */
class Arena {
public:
    /// @brief Пустая арена: ничего не выделяет.
    Arena() noexcept = default;

    /**
     * @brief Арена в собственном виртуальном резерве.
     * @param capacity    Предел роста, байт (резервируется сразу, подтверждается по мере надобности).
     * @param commit_step Шаг подтверждения физической памяти, байт.
     * @return Пустая арена, если ОС отказала в резерве.
     */
    [[nodiscard]] static Arena reserve(std::size_t capacity, std::size_t commit_step = KiB(64)) noexcept;

    /**
     * @brief Арена поверх внешнего буфера. Буфер зануляется один раз, здесь.
     * @warning Буфер должен жить дольше арены.
     */
    [[nodiscard]] static Arena over(std::span<std::byte> buffer) noexcept;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    ~Arena() = default;

    /**
     * @brief Выделяет `size` байт с выравниванием `alignment`. Память нулевая.
     * @pre `alignment` — степень двойки.
     * @return `nullptr`, если арена исчерпана (или пуста).
     */
    [[nodiscard]] void* push(std::size_t size, std::size_t alignment = default_alignment) noexcept;

    /// @brief Один нулевой объект `T`.
    template<ZeroInitializable T>
    [[nodiscard]] T* push() noexcept {
        return start_lifetime_as_array<T>(push(sizeof(T), alignof(T)), 1);
    }

    /// @brief Массив из `count` нулевых объектов `T`; пустой span, если места нет.
    template<ZeroInitializable T>
    [[nodiscard]] std::span<T> push_array(std::size_t count) noexcept {
        T* items = start_lifetime_as_array<T>(push(sizeof(T) * count, alignof(T)), count);
        return items != nullptr ? std::span<T>(items, count) : std::span<T>();
    }

    /// @brief Текущая позиция — для отката через pop_to().
    [[nodiscard]] ArenaMarker mark() const noexcept { return {m_position}; }

    /**
     * @brief Откатывает арену к отметке и зануляет освобождённое.
     * @pre Отметка получена от этой арены и не «из будущего» (не дальше текущей позиции).
     */
    void pop_to(ArenaMarker marker) noexcept;

    /// @brief Освобождает всё: откат к началу.
    void reset() noexcept { pop_to(ArenaMarker{}); }

    /**
     * @brief Возвращает ОС физическую память выше текущей позиции (только для reserve()).
     *
     * Полезно после пика: арена держит подтверждённой память под максимальное использование.
     */
    void shrink() noexcept;

    /// @brief `true`, если `pointer` указывает внутрь занятой части арены.
    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        const auto* p = static_cast<const std::byte*>(pointer);
        return m_base != nullptr && p >= m_base && p < m_base + m_position;
    }

    /// @brief Занято, байт.
    [[nodiscard]] std::size_t used() const noexcept { return m_position; }
    /// @brief Предел роста, байт.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    /// @brief Все счётчики.
    [[nodiscard]] ArenaStats stats() const noexcept { return {m_position, m_peak, m_committed, m_capacity}; }

private:
    bool grow_commit(std::size_t required_end) noexcept;

    VirtualRegion m_region;       ///< Пусто для арены над внешним буфером.
    std::byte* m_base = nullptr;
    std::size_t m_position = 0;   ///< [0, m_position) занято.
    std::size_t m_committed = 0;  ///< [m_position, m_committed) — нули (инвариант ZII).
    std::size_t m_capacity = 0;
    std::size_t m_peak = 0;
    std::size_t m_commit_step = 0;
};

/**
 * @brief Временная память: запоминает позицию арены и откатывает её в деструкторе.
 *
 * @code
 * void build_path(MemorySystem::Arena& scratch) {
 *     MemorySystem::ArenaScope scope(scratch);
 *     auto open = scratch.push_array<Node>(1024);
 *     ...
 * } // всё, что выделено внутри, освобождено и обнулено
 * @endcode
 */
class ArenaScope {
public:
    /// @brief Запоминает текущую позицию `arena`.
    explicit ArenaScope(Arena& arena) noexcept : m_arena(arena), m_marker(arena.mark()) {}
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;
    /// @brief Откатывает арену к запомненной позиции.
    ~ArenaScope() { m_arena.pop_to(m_marker); }

    /// @brief Арена, в которой открыта область.
    [[nodiscard]] Arena& arena() const noexcept { return m_arena; }

private:
    Arena& m_arena;
    ArenaMarker m_marker;
};

/**
 * @brief Две арены, которые меняются местами каждый тик.
 *
 * Та же модель, что у шины событий: всё, что выделено в тике N в current(), доступно
 * в тике N+1 через previous() и освобождается при следующем swap(). Удобно для данных,
 * которые живут ровно один тик после создания: пути, списки видимых объектов, ответы запросов.
 */
class DoubleArena {
public:
    /// @brief Пустая пара арен.
    DoubleArena() noexcept = default;

    /// @brief Две виртуальные арены по `capacity` байт.
    [[nodiscard]] static DoubleArena reserve(std::size_t capacity, std::size_t commit_step = KiB(64)) noexcept {
        DoubleArena pair;
        pair.m_arenas[0] = Arena::reserve(capacity, commit_step);
        pair.m_arenas[1] = Arena::reserve(capacity, commit_step);
        return pair;
    }

    /// @brief Арена текущего тика: сюда пишем.
    [[nodiscard]] Arena& current() noexcept { return m_arenas[m_current]; }
    /// @brief Арена прошлого тика: отсюда читаем.
    [[nodiscard]] const Arena& previous() const noexcept { return m_arenas[m_current ^ 1u]; }

    /// @brief Конец тика: прошлая арена очищается и становится текущей.
    void swap() noexcept {
        m_current ^= 1u;
        m_arenas[m_current].reset();
    }

private:
    Arena m_arenas[2];
    unsigned m_current = 0;
};

} // namespace MemorySystem
