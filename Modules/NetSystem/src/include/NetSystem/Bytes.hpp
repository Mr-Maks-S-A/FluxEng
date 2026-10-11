#pragma once
/**
 * @file Bytes.hpp
 * @brief Сериализация в байты: ByteWriter пишет, ByteReader читает с проверкой границ.
 *
 * Порядок байт — little-endian на всех платформах (поэтому формат одинаков на x86 и ARM). Читатель не бросает
 * исключений и не читает за границу: при нехватке данных он выставляет флаг ошибки и возвращает нули, а вызывающий
 * проверяет `ok()` один раз в конце разбора. Так враждебный пакет не может уронить приём.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace NetSystem {

static_assert(std::endian::native == std::endian::little, "NetSystem wire format is little-endian; add byte swaps for big-endian targets");

class ByteWriter {
public:
    /// @brief Пишет арифметическое значение или enum (float и double — побитово, IEEE 754).
    template<typename T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    ByteWriter& write(T value) {
        const std::size_t at = m_bytes.size();
        m_bytes.resize(at + sizeof(T));
        std::memcpy(m_bytes.data() + at, &value, sizeof(T));
        return *this;
    }

    /// @brief Пишет тривиально копируемую структуру целиком. Только для структур без padding и без указателей.
    template<typename T>
        requires(std::is_trivially_copyable_v<T> && !std::is_arithmetic_v<T> && !std::is_enum_v<T>)
    ByteWriter& write(const T& value) {
        write_bytes(std::as_bytes(std::span<const T, 1>(&value, 1)));
        return *this;
    }

    ByteWriter& write_bytes(std::span<const std::byte> data) {
        m_bytes.insert(m_bytes.end(), data.begin(), data.end());
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(m_bytes); }
    void clear() noexcept { m_bytes.clear(); }

private:
    std::vector<std::byte> m_bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : m_data(data) {}

    template<typename T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    [[nodiscard]] T read() noexcept {
        T value{};
        if (!take(&value, sizeof(T))) return T{};
        return value;
    }

    template<typename T>
        requires(std::is_trivially_copyable_v<T> && !std::is_arithmetic_v<T> && !std::is_enum_v<T>)
    [[nodiscard]] T read() noexcept {
        T value{};
        if (!take(&value, sizeof(T))) return T{};
        return value;
    }

    /// @brief Следующие `count` байт без копирования; при нехватке — пустой span и ошибка.
    [[nodiscard]] std::span<const std::byte> read_bytes(std::size_t count) noexcept {
        if (!m_ok || m_data.size() - m_pos < count) {
            m_ok = false;
            return {};
        }
        const auto view = m_data.subspan(m_pos, count);
        m_pos += count;
        return view;
    }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    /// @brief Прочитано без ошибок и до конца (нет лишних байт).
    [[nodiscard]] bool finished() const noexcept { return m_ok && m_pos == m_data.size(); }

private:
    bool take(void* out, std::size_t size) noexcept {
        if (!m_ok || m_data.size() - m_pos < size) {
            m_ok = false;
            return false;
        }
        std::memcpy(out, m_data.data() + m_pos, size);
        m_pos += size;
        return true;
    }

    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace NetSystem
