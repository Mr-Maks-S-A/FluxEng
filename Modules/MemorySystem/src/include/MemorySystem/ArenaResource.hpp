#pragma once
/**
 * @file ArenaResource.hpp
 * @brief Адаптер арены к `std::pmr::memory_resource`: стандартные контейнеры в арене.
 *
 * @code
 * MemorySystem::Arena scratch = MemorySystem::Arena::reserve(MemorySystem::MiB(16));
 * MemorySystem::ArenaResource resource(scratch);
 * std::pmr::vector<int> visible(&resource);   // растёт внутри арены
 * std::pmr::string name("orc", &resource);
 * @endcode
 *
 * Освобождение в адаптере — пустая операция: память возвращается откатом или reset() арены.
 * Поэтому контейнер, выросший в арене, оставляет в ней «хвосты» старых буферов — для временных
 * данных это нормально, для долгоживущих лучше заранее вызвать `reserve()`.
 */

#include <MemorySystem/Arena.hpp>

#include <memory_resource>
#include <new>

namespace MemorySystem {

/// @brief `std::pmr::memory_resource` поверх Arena.
class ArenaResource final : public std::pmr::memory_resource {
public:
    /// @warning Арена должна жить дольше ресурса и всех контейнеров, которые им пользуются.
    explicit ArenaResource(Arena& arena) noexcept : m_arena(&arena) {}

    /// @brief Арена, из которой берётся память.
    [[nodiscard]] Arena& arena() const noexcept { return *m_arena; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* memory = m_arena->push(bytes, alignment);
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return memory;
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    Arena* m_arena;
};

} // namespace MemorySystem
