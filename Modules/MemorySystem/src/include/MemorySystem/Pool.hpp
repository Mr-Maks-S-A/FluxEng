#pragma once
/**
 * @file Pool.hpp
 * @brief Пул объектов одного размера со свободным списком. Выдаёт обнулённые объекты.
 *
 * Для объектов, которые создаются и уничтожаются по одному и в произвольном порядке
 * (частицы, снаряды, узлы дерева), арены мало: отдельный объект из середины не освободить.
 * Пул хранит освобождённые блоки в односвязном списке прямо внутри них самих.
 *
 * **ZII.** Освобождённый блок зануляется (кроме указателя списка), выданный — полностью нулевой.
 * Блоки берутся из собственной виртуальной арены: адреса стабильны, пул не «переезжает».
 *
 * @code
 * auto particles = MemorySystem::Pool<Particle>::reserve(10'000);
 * Particle* p = particles.allocate();   // нулевой Particle
 * p->life = 2.0f;
 * particles.free(p);                    // блок обнулён и вернулся в список
 * @endcode
 */

#include <MemorySystem/Arena.hpp>
#include <MemorySystem/Core.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>

namespace MemorySystem {

/**
 * @brief Пул нулевых объектов `T`.
 *
 * Созданный по умолчанию пул пуст и валиден: allocate() возвращает `nullptr`.
 * @tparam T Тип объекта; нулевые байты — его корректное «пустое» состояние.
 */
template<ZeroInitializable T>
class Pool {
public:
    /// @brief Пустой пул.
    Pool() noexcept = default;

    /// @brief Пул не более чем на `max_items` одновременно живых объектов.
    [[nodiscard]] static Pool reserve(std::size_t max_items, std::size_t commit_step = KiB(64),
                                      MemoryTag tag = MemoryTag::Untagged) noexcept {
        Pool pool;
        pool.m_arena = Arena::reserve(std::max<std::size_t>(max_items, 1) * block_size, commit_step, tag);
        pool.m_capacity = max_items;
        return pool;
    }

    /// @brief Нулевой объект; `nullptr`, если пул исчерпан.
    [[nodiscard]] T* allocate() noexcept {
        void* block = nullptr;
        if (m_free != nullptr) {
            FreeNode* node = m_free;
            m_free = node->next;
            std::memset(static_cast<void*>(node), 0, sizeof(FreeNode)); // остальное обнулено при free()
            block = node;
        } else if (m_allocated < m_capacity) {
            block = m_arena.push(block_size, block_alignment);
            if (block == nullptr) {
                return nullptr;
            }
            ++m_allocated;
        } else {
            return nullptr;
        }
        ++m_live;
        return start_lifetime_as_array<T>(block, 1);
    }

    /**
     * @brief Возвращает объект в пул и зануляет его. `nullptr` допустим и ничего не делает.
     * @pre `item` получен из этого пула и ещё не освобождён.
     */
    void free(T* item) noexcept {
        if (item == nullptr) {
            return;
        }
        assert(owns(item) && "Pool::free: pointer does not belong to this pool");
        std::memset(static_cast<void*>(item), 0, block_size);
        m_free = ::new (static_cast<void*>(item)) FreeNode{m_free};
        --m_live;
    }

    /// @brief `true`, если `item` лежит в памяти пула.
    [[nodiscard]] bool owns(const T* item) const noexcept { return m_arena.owns(item); }
    /// @brief Живых объектов сейчас.
    [[nodiscard]] std::size_t live() const noexcept { return m_live; }
    /// @brief Предел одновременно живых объектов.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }

    /// @brief Размер блока: объект или узел списка, что больше, с выравниванием.
    static constexpr std::size_t block_alignment = std::max(alignof(T), alignof(void*));
    /// @brief Шаг между блоками в памяти пула.
    static constexpr std::size_t block_size = align_up(std::max(sizeof(T), sizeof(void*)), block_alignment);

private:
    struct FreeNode {
        FreeNode* next;
    };

    Arena m_arena;
    FreeNode* m_free = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_allocated = 0; ///< Блоков, когда-либо взятых из арены.
    std::size_t m_live = 0;
};

} // namespace MemorySystem
