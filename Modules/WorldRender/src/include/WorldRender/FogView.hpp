#pragma once
/**
 * @file FogView.hpp
 * @brief Туман: полупрозрачные билборды по ячейкам `FogSource`, яркость ячейки задаёт источник (для маны — плотность / база).
 *
 * Рисуются ячейки в радиусе от камеры, которые источник считает видимыми (яркость > 0). Что скрыть — решает источник:
 * готовый `ManaFogSource` (Adapters/Terrain.hpp) убирает ячейки в породе и выше 6 м над поверхностью — приземный слой.
 * Билборд складывается с фоном (Additive), глубину не пишет — дыра от заклинания видна как провал яркости.
 * Объёмный рендер по 3D-текстуре — позже, если билбордов окажется мало.
 */

#include <WorldRender/Camera.hpp>
#include <WorldRender/Sources.hpp>

#include <RendererSystem/RendererSystem.hpp>

#include <vector>

namespace WorldRender {

struct FogVertex {
    float center[3]; ///< От глаза.
    float corner[2]; ///< −1…1: смещение угла билборда.
    float density;   ///< Яркость 0…1+ (плотность / базовая).
};
static_assert(sizeof(FogVertex) == 24);

/// @brief Выбирает ячейки тумана в радиусе от `eye` и строит четыре вершины на каждую. Чистая функция: тестируется без GPU.
/// @return Число ячеек.
std::size_t build_fog_vertices(const FogSource& source, const glm::dvec3& eye, float radius, std::vector<FogVertex>& out);

class FogView {
public:
    explicit FogView(RendererSystem::RHI::Device& device);

    float radius = 36.0f;       ///< Дальность прорисовки, м.
    float intensity = 0.11f;    ///< Яркость одного билборда при базовой плотности.
    float size = 2.8f;          ///< Размер билборда, м (для ячейки 2 м — с перекрытием).

    /// @brief Обновляет вершины (при смене поля или заметном смещении камеры) и рисует.
    void draw(const FogSource& source, const View& view);
    [[nodiscard]] std::size_t cells() const noexcept { return m_cells; }

private:
    RendererSystem::RHI::Device* m_device;
    RendererSystem::Pipeline m_pipeline;
    RendererSystem::Mesh m_mesh;
    std::vector<FogVertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
    std::size_t m_cells = 0;
};

} // namespace WorldRender
