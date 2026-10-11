#pragma once
/**
 * @file Types.hpp
 * @brief Данные ассетов на стороне процессора: без GPU, окна и рендера (поэтому сервер тоже может их грузить).
 *
 * Клиент превращает их в GPU-ресурсы сам, в главном потоке (см. Sandbox/asset_lab):
 * ImageAsset → Texture, MeshAsset → Mesh.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AssetSystem {

/// @brief Картинка RGBA8, строки сверху вниз, без выравнивания строк.
struct ImageAsset {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; ///< `width * height * 4` байт.

    [[nodiscard]] std::size_t size_bytes() const noexcept { return rgba.size(); }
};

/// @brief Вершина сетки. Раскладка совпадает с RendererSystem::Vertex3D (36 байт), цвет — RGBA8 (R в младшем байте).
struct MeshVertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float uv[2] = {0.0f, 0.0f};
    std::uint32_t rgba = 0xFFFFFFFFu;
};
static_assert(sizeof(MeshVertex) == 36, "MeshVertex layout is part of the cooked mesh format");

/// @brief Треугольная сетка. Все примитивы glTF склеены в одну, преобразования узлов уже применены.
struct MeshAsset {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices; ///< Тройки индексов; каждый меньше `vertices.size()`.
    float bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float bounds_max[3] = {0.0f, 0.0f, 0.0f};
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Множитель цвета материала.
    std::string base_color_texture; ///< Путь текстуры в VFS (уже нормализован); пусто — без текстуры.

    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    [[nodiscard]] std::size_t size_bytes() const noexcept {
        return vertices.size() * sizeof(MeshVertex) + indices.size() * sizeof(std::uint32_t);
    }
    /// @brief Пересчитывает bounds_min/max по вершинам (для пустой сетки — нули).
    void compute_bounds() noexcept;
};

/// @brief Текст целиком (JSON данных заклинаний, конфиги, шейдеры).
struct TextAsset {
    std::string text;
    [[nodiscard]] std::size_t size_bytes() const noexcept { return text.size(); }
};

/// @brief Сырые байты (то, для чего нет своего загрузчика).
struct BlobAsset {
    std::vector<std::byte> bytes;
    [[nodiscard]] std::size_t size_bytes() const noexcept { return bytes.size(); }
};

} // namespace AssetSystem
