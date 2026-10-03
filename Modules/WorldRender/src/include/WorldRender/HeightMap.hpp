#pragma once
/**
 * @file HeightMap.hpp
 * @brief Карта высот для вида сверху: сетка «высота поверхности в клетке», частичное обновление и раскраска в изображение.
 *
 * Для игр с видом сверху (2D-карта над 3D-миром) и мини-карт. Модуль не знает про ландшафт: высоту отдаёт `HeightSource`
 * (готовый источник для SDF-ландшафта — `TerrainHeights` в `Adapters/Terrain.hpp`). После правки мира пересчитывается
 * только затронутый прямоугольник (`update`), а не вся карта; раскраска (`shade`) — отдельный дешёвый проход.
 *
 * @code
 * WorldRender::TerrainHeights source(world);
 * WorldRender::HeightMap map(128, 128, 1.0);
 * map.update(source);                                   // всё
 * map.update(source, TerrainHeights::cells_of_chunk(chunk, map)); // после правки
 * RendererSystem::Image picture = map.shade();          // рельеф с освещением, готов для update_texture
 * @endcode
 */

#include <RendererSystem/Core/Image.hpp>

#include <cmath>
#include <utility>
#include <vector>

namespace WorldRender {

/// @brief Откуда карта берёт высоту.
class HeightSource {
public:
    virtual ~HeightSource() = default;
    /// @brief Высота поверхности (м) над точкой (x, z) мира; `NaN` — поверхности нет. Только читает мир.
    [[nodiscard]] virtual double height_at(double x, double z) const = 0;
};

/// @brief Прямоугольник клеток карты: от `x0, z0` включительно до `x1, z1` не включая.
struct CellRegion {
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return x1 <= x0 || z1 <= z0; }
};

struct HeightShading {
    double sun_azimuth = -2.2;   ///< Откуда светит солнце (радианы в плоскости карты).
    double sun_elevation = 0.8;  ///< Высота солнца над горизонтом, радианы.
    double exaggeration = 1.4;   ///< Усиление рельефа в освещении.
    double low = 0.0;            ///< Высота, раскрашиваемая как «низина», м.
    double high = 40.0;          ///< Высота «вершин», м.
};

class HeightMap {
public:
    /// @param cell_meters Размер клетки, м. Клетка (i, j) — квадрат с углом (i·cell, j·cell).
    HeightMap(int width, int depth, double cell_meters = 1.0);

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int depth() const noexcept { return m_depth; }
    [[nodiscard]] double cell_meters() const noexcept { return m_cell; }
    /// @brief Высота клетки (`NaN` — нет поверхности). Вне карты — `NaN`.
    [[nodiscard]] float height(int x, int z) const noexcept;
    /// @brief Высота в точке мира с билинейной интерполяцией; `NaN` вне карты или без поверхности рядом.
    [[nodiscard]] double height_at(double x, double z) const noexcept;

    /// @brief Пересчитывает всю карту.
    void update(const HeightSource& source);
    /// @brief Пересчитывает прямоугольник клеток (обрезается по карте). Возвращает число пересчитанных клеток.
    int update(const HeightSource& source, CellRegion region);
    /// @brief Какие клетки покрывает квадратная область мира [x, x+size) × [z, z+size).
    [[nodiscard]] CellRegion cells_of(double x, double z, double size) const noexcept;

    /// @brief Наименьшая и наибольшая высота среди клеток с поверхностью ({0, 1}, если поверхности нигде нет): для раскраски по фактическому диапазону.
    [[nodiscard]] std::pair<double, double> range() const noexcept;

    /// @brief Рельеф с освещением и цветом по высоте (RGBA8, `width × depth`, строка 0 — z = 0).
    [[nodiscard]] RendererSystem::Image shade(const HeightShading& shading = {}) const;

private:
    int m_width, m_depth;
    double m_cell;
    std::vector<float> m_height;
};

} // namespace WorldRender
