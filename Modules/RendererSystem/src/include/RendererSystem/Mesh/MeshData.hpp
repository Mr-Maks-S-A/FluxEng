#pragma once
/**
 * @file MeshData.hpp
 * @brief 3D-сетка в оперативной памяти (без OpenGL) и генераторы примитивов.
 */

#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Geometry3D.hpp>
#include <RendererSystem/RHI/Types.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RendererSystem {

/**
 * @brief Стандартная вершина 3D-рендера (36 байт).
 *
 * UV — по соглашению модуля: (0, 0) — левый верх изображения.
 */
struct Vertex3D {
    glm::vec3 position{0.0f};          ///< Позиция.
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; ///< Нормаль (единичная).
    glm::vec2 uv{0.0f};                ///< Текстурные координаты.
    Color color = Colors::white;       ///< Цвет вершины (умножается на материал).
};

static_assert(sizeof(Vertex3D) == 36, "Vertex3D layout is part of the GPU contract");

/// @brief Раскладка Vertex3D для конвейеров: 0 — позиция, 1 — нормаль, 2 — UV, 3 — цвет.
[[nodiscard]] constexpr RHI::VertexLayout vertex3d_layout() {
    return RHI::VertexLayout::make(sizeof(Vertex3D), {{0, 3, RHI::AttributeType::Float, offsetof(Vertex3D, position)},
                                                      {1, 3, RHI::AttributeType::Float, offsetof(Vertex3D, normal)},
                                                      {2, 2, RHI::AttributeType::Float, offsetof(Vertex3D, uv)},
                                                      {3, 4, RHI::AttributeType::UnsignedByteNorm, offsetof(Vertex3D, color)}});
}

/**
 * @brief Индексированная сетка треугольников: вершины + индексы (против часовой стрелки — лицевая сторона).
 *
 * Генераторы строят сетку с центром в начале координат; положение и размер задаёт матрица модели.
 * Сетки можно склеивать (append) — так несколько примитивов рисуются одним вызовом.
 */
struct MeshData {
    std::vector<Vertex3D> vertices;      ///< Вершины.
    std::vector<std::uint32_t> indices;  ///< Тройки индексов.

    /// @brief Количество треугольников.
    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    /// @brief Коробка, охватывающая все вершины (пустая сетка — нулевая коробка).
    [[nodiscard]] Aabb bounds() const noexcept;
    /// @brief Удаляет всё (память сохраняется).
    void clear() noexcept;

    /// @brief Добавляет другую сетку, преобразованную матрицей (нормали — обратной транспонированной).
    void append(const MeshData& other, const glm::mat4& transform = glm::mat4{1.0f});
    /// @brief Заливает все вершины цветом.
    void set_color(Color color) noexcept;
    /// @brief Пересчитывает гладкие нормали по треугольникам.
    void compute_normals();

    /// @brief Коробка размера `size`; у каждой грани свои нормали и UV 0…1.
    [[nodiscard]] static MeshData box(glm::vec3 size = glm::vec3{1.0f});
    /// @brief Прямоугольник в плоскости XY, лицом к +Z; UV (0,0) — левый верх.
    [[nodiscard]] static MeshData quad(glm::vec2 size = glm::vec2{1.0f});
    /// @brief Плоскость XZ лицом вверх (+Y), разбитая на `segments` клеток; UV растянуты на всю плоскость.
    [[nodiscard]] static MeshData plane(glm::vec2 size = glm::vec2{1.0f}, glm::ivec2 segments = {1, 1});
    /// @brief Сфера (UV-сфера): `segments` по долготе, `rings` по широте.
    [[nodiscard]] static MeshData sphere(float radius = 0.5f, int segments = 24, int rings = 16);
    /// @brief Цилиндр вдоль Y с крышками; UV крышек — проекция сверху (круг вписан в 0…1).
    [[nodiscard]] static MeshData cylinder(float radius = 0.5f, float height = 1.0f, int segments = 32);
    /// @brief Плоский прямоугольник со скруглёнными углами в плоскости XY лицом к +Z (UV по описанному прямоугольнику).
    [[nodiscard]] static MeshData rounded_rect(glm::vec2 size, float radius, int corner_segments = 6);
    /**
     * @brief Пластина со скруглёнными углами — карта, жетон, плитка.
     *
     * Лицевая сторона смотрит в +Z, оборотная — в −Z (её UV отражены по X, чтобы рисунок не был зеркальным),
     * толщина — по Z. UV обеих сторон — по описанному прямоугольнику.
     */
    [[nodiscard]] static MeshData rounded_slab(glm::vec2 size, float thickness, float radius, int corner_segments = 6);
};

} // namespace RendererSystem
