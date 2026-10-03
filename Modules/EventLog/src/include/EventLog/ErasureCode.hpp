#pragma once
/**
 * @file ErasureCode.hpp
 * @brief Код Рида—Соломона (матрица Коши над GF(2⁸)): из `k` блоков данных получаем `m` блоков чётности,
 * после чего любые `m` потерянных блоков из `k + m` восстанавливаются.
 *
 * Это обобщение чётности RAID-5 (m = 1: потеря одного блока) и RAID-6 (m = 2). Код **систематический**: блоки данных
 * хранятся как есть, чётность — отдельные блоки, поэтому при отсутствии повреждений читать можно без декодирования.
 *
 * Потери должны быть **известны** (стираются блоки, у которых не сошлась контрольная сумма): код с `m` блоками чётности
 * исправляет ровно `m` известных стираний. Поиск повреждённых блоков — задача контрольных сумм (`Math::crc32c`), см. Journal.hpp.
 *
 * @code
 * EventLog::ErasureCode code(8, 2);                           // 8 блоков данных, 2 блока чётности
 * code.encode(data_blocks, parity_blocks);                  // k одинаковых по размеру блоков → m блоков чётности
 * // … два блока потерялись …
 * bool ok = code.reconstruct(all_blocks, present);         // восстанавливает потерянные на месте
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace EventLog {

class ErasureCode {
public:
    /// @param data_blocks k ≥ 1; @param parity_blocks m ≥ 1; k + m ≤ 256 (размер поля GF(2⁸)).
    ErasureCode(int data_blocks, int parity_blocks);

    [[nodiscard]] int data_blocks() const noexcept { return m_k; }
    [[nodiscard]] int parity_blocks() const noexcept { return m_m; }

    /// @brief Считает `m` блоков чётности по `k` блокам данных. Все блоки одного размера (≥ 1 байта).
    void encode(std::span<const std::span<const std::byte>> data, std::span<const std::span<std::byte>> parity) const;

    /**
     * @brief Восстанавливает потерянные блоки на месте.
     * @param blocks `k + m` блоков одного размера: сначала данные, затем чётность. Содержимое отсутствующих не важно.
     * @param present `present[i] == false` — блок `i` потерян или повреждён.
     * @return `false`, если потеряно больше `m` блоков (ничего не изменено); иначе все блоки приведены в порядок.
     */
    [[nodiscard]] bool reconstruct(std::span<const std::span<std::byte>> blocks, std::span<const bool> present) const;

private:
    int m_k, m_m;
    std::vector<unsigned char> m_matrix; ///< Матрица Коши m × k: коэффициенты чётности.
};

/// @brief Арифметика поля GF(2⁸) (полином 0x11D): отдельно, чтобы её можно было проверить тестами.
namespace gf256 {
[[nodiscard]] unsigned char mul(unsigned char a, unsigned char b) noexcept;
/// @brief Обратный элемент; для 0 возвращает 0 (деления на ноль в коде не бывает: матрица Коши не вырождена).
[[nodiscard]] unsigned char inv(unsigned char a) noexcept;
/// @brief dst[i] ^= coefficient · src[i] — основная операция кодирования.
void mul_add(std::span<std::byte> dst, std::span<const std::byte> src, unsigned char coefficient) noexcept;
} // namespace gf256

} // namespace EventLog
