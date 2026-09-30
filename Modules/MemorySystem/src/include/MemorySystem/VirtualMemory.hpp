#pragma once
/**
 * @file VirtualMemory.hpp
 * @brief Резерв и подтверждение виртуальной памяти ОС (Windows: VirtualAlloc, POSIX: mmap).
 *
 * Идея: зарезервировать большой непрерывный диапазон адресов сразу (например, 1 ГиБ),
 * а физическую память подтверждать (commit) по мере роста. Резерв ничего не стоит,
 * адреса не меняются, и всё, что лежит в диапазоне, остаётся на своих местах —
 * указатели в арену никогда не «переезжают», как это бывает у `std::vector`.
 *
 * Подтверждённые страницы ОС отдаёт **обнулёнными** — ZII получается бесплатно.
 */

#include <cstddef>

namespace MemorySystem {

/// @brief Размер страницы ОС (обычно 4 КиБ).
[[nodiscard]] std::size_t page_size() noexcept;

/// @brief Гранулярность резерва (Windows: 64 КиБ, POSIX: размер страницы).
[[nodiscard]] std::size_t reservation_granularity() noexcept;

/**
 * @brief Зарезервированный диапазон виртуальных адресов (RAII).
 *
 * Нулевой (созданный по умолчанию) VirtualRegion пуст и валиден — это ZII:
 * `data() == nullptr`, `reserved() == 0`, освобождать нечего.
 */
class VirtualRegion {
public:
    /// @brief Пустой регион.
    VirtualRegion() noexcept = default;

    /**
     * @brief Резервирует не меньше `bytes` байт адресного пространства (без физической памяти).
     * @return Пустой регион, если ОС отказала.
     */
    [[nodiscard]] static VirtualRegion reserve(std::size_t bytes) noexcept;

    VirtualRegion(const VirtualRegion&) = delete;
    VirtualRegion& operator=(const VirtualRegion&) = delete;
    /// @brief Перемещение: резерв переходит к новому объекту, старый становится пустым.
    VirtualRegion(VirtualRegion&& other) noexcept;
    /// @copydoc VirtualRegion(VirtualRegion&&)
    VirtualRegion& operator=(VirtualRegion&& other) noexcept;
    ~VirtualRegion();

    /**
     * @brief Подтверждает страницы, покрывающие `[offset, offset + bytes)`. Новые страницы — нулевые.
     * @return `false`, если ОС отказала или диапазон выходит за резерв.
     */
    bool commit(std::size_t offset, std::size_t bytes) noexcept;

    /**
     * @brief Возвращает ОС физическую память страниц, целиком лежащих в `[offset, offset + bytes)`.
     *
     * Адреса остаются зарезервированными. После повторного commit страницы снова нулевые.
     */
    void decommit(std::size_t offset, std::size_t bytes) noexcept;

    /// @brief Начало региона (или `nullptr`).
    [[nodiscard]] std::byte* data() const noexcept { return m_base; }
    /// @brief Размер резерва, байт (кратен гранулярности).
    [[nodiscard]] std::size_t reserved() const noexcept { return m_reserved; }
    /// @brief `true`, если регион что-то зарезервировал.
    [[nodiscard]] explicit operator bool() const noexcept { return m_base != nullptr; }

private:
    void release() noexcept;

    std::byte* m_base = nullptr;
    std::size_t m_reserved = 0;
};

} // namespace MemorySystem
