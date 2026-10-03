#pragma once
/**
 * @file TerrainView.hpp
 * @brief Поверхность мира на экране: очередь перестройки сеток, загрузка на GPU, освещение по нормалям.
 *
 * - `MeshQueue` — чанки, ждущие перестройки (без повторов), ближние к камере — первыми;
 * - `TerrainView::update` — за кадр строит не больше `budget` сеток (параллельно, JobSystem) и грузит их на GPU;
 * - данные берутся у `SurfaceSource` (интерфейс, см. Sources.hpp): ландшафт, планета, тестовая сетка — рендеру всё равно;
 * - один направленный свет, нормали из градиента SDF, цвет по высоте и уклону, текстур нет;
 * - каркасный режим и рамки чанков для отладки.
 *
 * Типы графического API наружу не выходят: всё через RHI из RendererSystem.
 */

#include <WorldRender/Camera.hpp>
#include <WorldRender/DebugDraw.hpp>
#include <WorldRender/Sources.hpp>

#include <JobSystem/JobSystem.hpp>
#include <RendererSystem/RendererSystem.hpp>

#include <array>
#include <vector>

namespace WorldRender {

/// @brief Очередь чанков на перестройку меша.
class MeshQueue {
public:
    explicit MeshQueue(const GridShape& shape = {}) : m_shape(shape), m_queued(static_cast<std::size_t>(shape.chunk_count()), 0) {}
    /// @brief Ставит чанк в очередь; повторная постановка ничего не меняет.
    void push(ChunkIndex c);
    void push(std::span<const ChunkIndex> chunks);
    /// @brief Забирает до `count` чанков, ближайших к `eye` (метры мира).
    [[nodiscard]] std::vector<ChunkIndex> pop_nearest(std::size_t count, const glm::dvec3& eye);
    [[nodiscard]] std::size_t size() const noexcept { return m_queue.size(); }
    [[nodiscard]] bool contains(ChunkIndex c) const noexcept { return m_queued[static_cast<std::size_t>(m_shape.index(c))] != 0; }

private:
    GridShape m_shape;
    std::vector<ChunkIndex> m_queue;
    std::vector<char> m_queued;
};

struct Light {
    glm::vec3 direction{-0.45f, -0.8f, -0.35f}; ///< Куда светит солнце.
    glm::vec3 color{1.0f, 0.95f, 0.85f};
    glm::vec3 ambient{0.30f, 0.34f, 0.42f};
    glm::vec3 fog_color{0.55f, 0.70f, 0.90f};
    float fog_start = 70.0f, fog_end = 220.0f;
};

struct TerrainStats {
    std::size_t queued = 0;        ///< Чанков ждёт перестройки.
    std::uint32_t built = 0;       ///< Сеток построено за последний update.
    double build_ms = 0.0;         ///< Время последнего update (построение + загрузка).
    std::uint32_t drawn = 0;       ///< Чанков нарисовано в последнем кадре.
    std::uint32_t culled = 0;      ///< Отсечено пирамидой видимости.
    std::size_t triangles = 0;     ///< Треугольников во всех сетках.
    std::size_t gpu_bytes = 0;
};

class TerrainView {
public:
    /// @param source Источник поверхности; должен жить дольше `TerrainView`.
    TerrainView(RendererSystem::RHI::Device& device, const SurfaceSource& source);
    TerrainView(const TerrainView&) = delete;
    TerrainView& operator=(const TerrainView&) = delete;

    /// @brief Ставит чанки в очередь (обычно изменённые чанки мира после тика).
    void enqueue(std::span<const ChunkIndex> chunks) { m_queue.push(chunks); }
    /**
     * @brief Строит до `budget` сеток из очереди (ближние к `eye` — первыми), грузит на GPU.
     * `budget = 0` — всё, что есть (стартовый мир). Вызывать из потока устройства.
     */
    void update(JobSystem::Scheduler& jobs, const glm::dvec3& eye, std::size_t budget = 4);

    /// @brief Рисует чанки в пирамиде видимости. Камера — относительная (`View::camera`).
    void draw(const View& view, const Light& light);

    void set_wireframe(bool on) noexcept { m_wireframe = on; }
    [[nodiscard]] bool wireframe() const noexcept { return m_wireframe; }
    /// @brief Рамки всех чанков в линиях (для отладки).
    static void add_chunk_boxes(DebugDraw& lines, const GridShape& shape, RendererSystem::Color color);

    [[nodiscard]] const TerrainStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const MeshQueue& queue() const noexcept { return m_queue; }

private:
    struct Slot {
        RendererSystem::Mesh solid;
        RendererSystem::Mesh wire;
        SurfaceMesh cpu;          ///< Для каркаса: индексы линий строятся из неё по требованию.
        bool wire_ready = false;
        bool has_mesh = false;
    };

    RendererSystem::RHI::Device* m_device;
    RendererSystem::Pipeline m_solid;
    RendererSystem::Pipeline m_lines;
    const SurfaceSource* m_source;
    GridShape m_shape;
    std::vector<Slot> m_slots;
    MeshQueue m_queue;
    TerrainStats m_stats;
    bool m_wireframe = false;
};

} // namespace WorldRender
