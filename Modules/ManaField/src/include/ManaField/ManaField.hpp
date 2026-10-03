#pragma once
/**
 * @file ManaField.hpp
 * @brief Поле маны в безграничном мире: разреженная сетка чанков 16³ ячеек (ячейка 2 м, чанк 32 м).
 *
 * Поле существует везде, но хранится только там, где оно отличается от базового уровня:
 * - **виртуальная база**: незагруженная ячейка равна `Config::base`; новое поле не занимает памяти;
 * - **чанки по требованию**: `draw` / `inject` создают чанки, шаг поля создаёт «гало» — соседей по граням
 *   активных чанков (диффузия идёт по шести соседям) — и удаляет чанк, когда все его ячейки вернулись к базе;
 * - чанк поля (32 м) выровнен по сетке чанков ландшафта (16 м): один чанк поля = 2×2×2 чанка `Terrain`,
 *   поэтому оба хранилища можно подгружать и выгружать одной картой чанков;
 * - координаты ячеек — int64: мир ограничен только `WorldPos` (≈ 940 а.е.).
 *
 * Шаг (10 раз в секунду):
 * - диффузия считается потоками между парами соседних ячеек: сколько вычли из одной, ровно столько прибавили к другой,
 *   в том числе через границу чанков — `excess_raw()` (Σ (ячейка − база)) сохраняется точно, пока выключен возврат к базе;
 * - возврат к базе — единственный источник маны; каждый шаг сдвигает ячейку хотя бы на единицу, поэтому поле
 *   сходится к базе ровно, и чанки освобождаются.
 *
 * Порядок обхода — по возрастанию ключа чанка (`std::map`), хеш не зависит от истории выделения чанков.
 */

#include <Math/Fixed.hpp>
#include <Math/Mana.hpp>
#include <Math/Vec.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace ManaField {

constexpr int chunk_cells = 16;
constexpr int chunk_volume = chunk_cells * chunk_cells * chunk_cells;
/// @brief Сторона ячейки: 2 м в единицах WorldPos.
constexpr std::int64_t cell_raw = 2 * Math::Fixed::one_raw;
/// @brief Сторона чанка: 32 м.
constexpr std::int64_t chunk_raw = chunk_cells * cell_raw;

/// @brief Константы поля. Время затягивания дыры настраивается здесь.
struct Config {
    Math::Fixed diffusion = Math::Fixed::from_ratio(1, 128); ///< Доля разности, уходящая в соседа за шаг (≤ 1/6).
    Math::Fixed relax = Math::Fixed::from_ratio(1, 80);      ///< Доля отклонения от базы, возвращаемая за шаг.
    Math::Mana base = Math::Mana::from_int(40);              ///< Базовая плотность ячейки.
};

/// @brief Индексы ячейки в мире (могут быть отрицательными).
struct CellPos {
    std::int64_t x = 0, y = 0, z = 0;
    [[nodiscard]] friend constexpr bool operator==(CellPos, CellPos) noexcept = default;
};
struct ChunkKey {
    std::int64_t x = 0, y = 0, z = 0;
    [[nodiscard]] friend constexpr auto operator<=>(const ChunkKey&, const ChunkKey&) noexcept = default;
};

/// @brief Деление с округлением к минус бесконечности (для отрицательных координат).
[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
[[nodiscard]] constexpr CellPos cell_of(Math::WorldPos p) noexcept { return {floor_div(p.x, cell_raw), floor_div(p.y, cell_raw), floor_div(p.z, cell_raw)}; }
[[nodiscard]] constexpr ChunkKey chunk_of(CellPos c) noexcept { return {floor_div(c.x, chunk_cells), floor_div(c.y, chunk_cells), floor_div(c.z, chunk_cells)}; }
/// @brief Индекс ячейки внутри чанка по локальным координатам 0…15.
[[nodiscard]] constexpr std::size_t local_index(int x, int y, int z) noexcept { return static_cast<std::size_t>((y * chunk_cells + z) * chunk_cells + x); }

class ManaGrid {
public:
    /// @brief Пустое поле: везде базовая плотность, ни одного чанка.
    explicit ManaGrid(const Config& config = {});

    [[nodiscard]] const Config& config() const noexcept { return m_config; }

    /// @brief Один шаг поля (10 раз в секунду: каждый шестой тик симуляции).
    void step();

    /// @brief Плотность ячейки, в которой лежит точка (вне чанков — база).
    [[nodiscard]] Math::Mana density(Math::WorldPos pos) const noexcept { return at(cell_of(pos)); }
    [[nodiscard]] Math::Mana at(CellPos cell) const noexcept;
    [[nodiscard]] Math::Mana at(std::int64_t x, std::int64_t y, std::int64_t z) const noexcept { return at(CellPos{x, y, z}); }

    /**
     * @brief Забирает ману из ячеек, центры которых лежат в `radius` от точки, пропорционально их плотности.
     * @return Сколько удалось забрать (≤ amount, ≤ доступного); ячейка не уходит ниже нуля.
     */
    Math::Mana draw(Math::WorldPos pos, Math::Fixed radius, Math::Mana amount);
    /// @brief Добавляет ману в ячейку точки. Ячейка не переполняется выше Fixed::max().
    void inject(Math::WorldPos pos, Math::Mana amount);

    /// @brief Σ (ячейка − база) по загруженным чанкам, в Fixed.raw: сохраняется диффузией (включая границы чанков).
    [[nodiscard]] std::int64_t excess_raw() const noexcept;
    /// @brief Сколько чанков держит поле сейчас (0 — всё в базовом состоянии).
    [[nodiscard]] std::size_t allocated_chunks() const noexcept { return m_chunks.size(); }
    /// @brief Хеш поля; не зависит от того, какие «пустые» чанки сейчас выделены; пересчитывается только после изменений.
    [[nodiscard]] std::uint64_t hash() const;
    /// @brief Растёт при каждом изменении поля (рендер перестраивает туман только по нему).
    [[nodiscard]] std::uint32_t version() const noexcept { return m_version; }

    /// @brief Обход загруженных чанков по возрастанию ключа: ячейки чанка (сырой формат хранения Fixed) в порядке `local_index`.
    template<typename Fn>
    void for_each_chunk(Fn&& fn) const {
        for (const auto& [key, chunk] : m_chunks) fn(key, std::span<const Math::Fixed>(chunk->cur));
    }

private:
    struct Chunk {
        std::array<Math::Fixed, chunk_volume> cur;
        std::array<Math::Fixed, chunk_volume> next;
    };
    Chunk& ensure(const ChunkKey& key);
    Math::Fixed& cell_ref(CellPos cell);
    [[nodiscard]] bool settled(const Chunk& chunk) const noexcept;

    Config m_config;
    std::map<ChunkKey, std::unique_ptr<Chunk>> m_chunks;
    std::uint32_t m_version = 1;
    mutable std::uint32_t m_hashed_version = 0;
    mutable std::uint64_t m_hash = 0;
};

} // namespace ManaField
