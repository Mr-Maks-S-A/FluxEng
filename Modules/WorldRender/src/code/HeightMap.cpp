#include <WorldRender/HeightMap.hpp>

#include <algorithm>
#include <limits>

namespace WorldRender {

namespace {
constexpr float nan = std::numeric_limits<float>::quiet_NaN();
}

HeightMap::HeightMap(int width, int depth, double cell_meters)
    : m_width(std::max(width, 1)), m_depth(std::max(depth, 1)), m_cell(cell_meters), m_height(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_depth), nan) {}

float HeightMap::height(int x, int z) const noexcept {
    if (x < 0 || z < 0 || x >= m_width || z >= m_depth) return nan;
    return m_height[static_cast<std::size_t>(z) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x)];
}

double HeightMap::height_at(double x, double z) const noexcept {
    // Узлы интерполяции — центры клеток.
    const double fx = x / m_cell - 0.5, fz = z / m_cell - 0.5;
    const int ix = static_cast<int>(std::floor(fx)), iz = static_cast<int>(std::floor(fz));
    const double tx = fx - ix, tz = fz - iz;
    double sum = 0.0, weight = 0.0;
    for (int dz = 0; dz <= 1; ++dz) {
        for (int dx = 0; dx <= 1; ++dx) {
            const float h = height(std::clamp(ix + dx, 0, m_width - 1), std::clamp(iz + dz, 0, m_depth - 1));
            if (std::isnan(h)) continue;
            const double w = (dx ? tx : 1.0 - tx) * (dz ? tz : 1.0 - tz);
            sum += w * static_cast<double>(h), weight += w;
        }
    }
    if (x < 0.0 || z < 0.0 || x > m_width * m_cell || z > m_depth * m_cell || weight <= 0.0) return static_cast<double>(nan);
    return sum / weight;
}

void HeightMap::update(const HeightSource& source) { update(source, {0, 0, m_width, m_depth}); }

int HeightMap::update(const HeightSource& source, CellRegion region) {
    region = {std::max(region.x0, 0), std::max(region.z0, 0), std::min(region.x1, m_width), std::min(region.z1, m_depth)};
    if (region.empty()) return 0;
    for (int z = region.z0; z < region.z1; ++z) {
        for (int x = region.x0; x < region.x1; ++x) {
            const double h = source.height_at((x + 0.5) * m_cell, (z + 0.5) * m_cell);
            m_height[static_cast<std::size_t>(z) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x)] = std::isnan(h) ? nan : static_cast<float>(h);
        }
    }
    return (region.x1 - region.x0) * (region.z1 - region.z0);
}

CellRegion HeightMap::cells_of(double x, double z, double size) const noexcept {
    return {static_cast<int>(std::floor(x / m_cell)), static_cast<int>(std::floor(z / m_cell)), static_cast<int>(std::ceil((x + size) / m_cell)), static_cast<int>(std::ceil((z + size) / m_cell))};
}

std::pair<double, double> HeightMap::range() const noexcept {
    double lo = 1e30, hi = -1e30;
    for (const float h : m_height) {
        if (std::isnan(h)) continue;
        lo = std::min(lo, static_cast<double>(h)), hi = std::max(hi, static_cast<double>(h));
    }
    if (lo > hi) return {0.0, 1.0};
    return {lo, std::max(hi, lo + 1.0)};
}

RendererSystem::Image HeightMap::shade(const HeightShading& s) const {
    using RendererSystem::Color;
    RendererSystem::Image image(m_width, m_depth, Color{12, 14, 24, 255});
    const double lx = std::cos(s.sun_azimuth) * std::cos(s.sun_elevation), lz = std::sin(s.sun_azimuth) * std::cos(s.sun_elevation), ly = std::sin(s.sun_elevation);
    for (int z = 0; z < m_depth; ++z) {
        for (int x = 0; x < m_width; ++x) {
            const float h = height(x, z);
            if (std::isnan(h)) continue;
            // Наклон по разностям с соседями (на краях и у «дыр» берём саму клетку).
            const auto at = [&](int ox, int oz) { const float v = height(x + ox, z + oz); return std::isnan(v) ? h : v; };
            const double gx = (at(1, 0) - at(-1, 0)) / (2.0 * m_cell) * s.exaggeration, gz = (at(0, 1) - at(0, -1)) / (2.0 * m_cell) * s.exaggeration;
            const double inv = 1.0 / std::sqrt(gx * gx + gz * gz + 1.0);
            const double lit = std::clamp((-gx * lx - gz * lz + ly) * inv, 0.0, 1.0);
            const double shade = 0.35 + 0.75 * lit;
            // Цвет по высоте: трава → земля → камень → снег.
            const double t = std::clamp((static_cast<double>(h) - s.low) / std::max(s.high - s.low, 1e-6), 0.0, 1.0);
            const double r = t < 0.5 ? 70 + 130 * t : 135 + 120 * (t - 0.5), g = t < 0.5 ? 130 - 50 * t : 105 + 130 * (t - 0.5), b = t < 0.5 ? 60 + 40 * t : 80 + 170 * (t - 0.5);
            image.set_pixel(x, z, Color{static_cast<std::uint8_t>(std::clamp(r * shade, 0.0, 255.0)), static_cast<std::uint8_t>(std::clamp(g * shade, 0.0, 255.0)),
                                        static_cast<std::uint8_t>(std::clamp(b * shade, 0.0, 255.0)), 255});
        }
    }
    return image;
}

} // namespace WorldRender
