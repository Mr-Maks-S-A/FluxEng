#pragma once
/**
 * @file DebugDraw.hpp
 * @brief Отладочные линии: накапливаются за кадр в мировых координатах (double) и рисуются одним вызовом.
 */

#include <WorldRender/Camera.hpp>

#include <RendererSystem/RendererSystem.hpp>

#include <vector>

namespace WorldRender {

struct LineVertex {
    float position[3];
    std::uint8_t rgba[4];
};
static_assert(sizeof(LineVertex) == 16);

class DebugDraw {
public:
    void line(const glm::dvec3& a, const glm::dvec3& b, RendererSystem::Color color);
    /// @brief Рёбра параллелепипеда.
    void box(const glm::dvec3& min, const glm::dvec3& max, RendererSystem::Color color);
    /// @brief Крест из трёх осей вокруг точки.
    void cross(const glm::dvec3& at, double half_size, RendererSystem::Color color);

    [[nodiscard]] std::size_t line_count() const noexcept { return m_lines.size() / 2; }
    [[nodiscard]] bool empty() const noexcept { return m_lines.empty(); }
    void clear() noexcept { m_lines.clear(); }
    [[nodiscard]] const std::vector<std::pair<glm::dvec3, std::uint32_t>>& raw() const noexcept { return m_lines; }

private:
    std::vector<std::pair<glm::dvec3, std::uint32_t>> m_lines; ///< Концы отрезков парами и цвет RGBA8.
};

/// @brief Рисовальщик линий DebugDraw на RHI: свой конвейер и поточный буфер.
class LineRenderer {
public:
    explicit LineRenderer(RendererSystem::RHI::Device& device);
    /// @brief Рисует накопленные линии относительно камеры и очищает `lines`.
    void flush(DebugDraw& lines, const View& view);

private:
    RendererSystem::RHI::Device* m_device;
    RendererSystem::Pipeline m_pipeline;
    RendererSystem::Mesh m_mesh;
    std::vector<LineVertex> m_scratch;
};

} // namespace WorldRender
