#pragma once
/**
 * @file ColumnBuffer.hpp
 * @brief Нетипизированный выровненный массив элементов фиксированного размера.
 */

#include <cstddef>

namespace EventSystem {

/**
 * @brief Одна колонка данных: сырая выровненная память под `capacity` элементов по `stride` байт.
 *
 * Аналог VBO: колонка ничего не знает о типе, только о размере и выравнивании элемента.
 * Количество живых элементов хранит владелец (EventBuffer), поэтому у нескольких
 * колонок одного буфера размер всегда общий.
 *
 * Начало колонки всегда выровнено минимум по #base_alignment (кэш-линия):
 * колонки не делят кэш-линии между собой, а SIMD-обработка может начинаться
 * с выровненного адреса. Элементы внутри колонки выровнены по `stride`.
 *
 * @note `alignas` на члене структуры не меняет выравнивание типа этого члена
 *       (`alignas(32) float v[8]` — это всё ещё `float[8]` с выравниванием 4).
 *       Поэтому для SoA-колонок полезно именно выравнивание начала колонки.
 */
class ColumnBuffer {
public:
    /// @brief Минимальное выравнивание начала колонки, байт.
    static constexpr std::size_t base_alignment = 64;

    /**
     * @param stride    Размер одного элемента, байт (> 0, кратен alignment).
     * @param alignment Выравнивание элемента, байт (степень двойки).
     */
    ColumnBuffer(std::size_t stride, std::size_t alignment) noexcept;
    ~ColumnBuffer();

    ColumnBuffer(const ColumnBuffer&) = delete;
    ColumnBuffer& operator=(const ColumnBuffer&) = delete;
    ColumnBuffer(ColumnBuffer&& other) noexcept;
    ColumnBuffer& operator=(ColumnBuffer&& other) noexcept;

    /**
     * @brief Перевыделяет память под `new_capacity` элементов, сохраняя первые `preserved_count`.
     * @throws std::bad_alloc, std::length_error — при нехватке памяти или переполнении размера.
     *         При исключении колонка остаётся в прежнем состоянии.
     */
    void reallocate(std::size_t new_capacity, std::size_t preserved_count);

    /// @brief Указатель на элемент `index` (без проверки границ).
    [[nodiscard]] std::byte* at(std::size_t index) noexcept { return m_data + index * m_stride; }
    /// @copydoc at
    [[nodiscard]] const std::byte* at(std::size_t index) const noexcept { return m_data + index * m_stride; }

    /// @brief Начало данных (`nullptr`, если память не выделена).
    [[nodiscard]] std::byte* data() noexcept { return m_data; }
    /// @copydoc data
    [[nodiscard]] const std::byte* data() const noexcept { return m_data; }

    /// @brief Размер элемента, байт.
    [[nodiscard]] std::size_t stride() const noexcept { return m_stride; }
    /// @brief Выравнивание элемента, байт.
    [[nodiscard]] std::size_t alignment() const noexcept { return m_alignment; }
    /// @brief Вместимость в элементах.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    /// @brief Выделено памяти, байт.
    [[nodiscard]] std::size_t allocated_bytes() const noexcept { return m_capacity * m_stride; }

private:
    [[nodiscard]] std::size_t allocation_alignment() const noexcept {
        return m_alignment > base_alignment ? m_alignment : base_alignment;
    }
    void release() noexcept;

    std::byte* m_data = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_stride;
    std::size_t m_alignment;
};

} // namespace EventSystem
