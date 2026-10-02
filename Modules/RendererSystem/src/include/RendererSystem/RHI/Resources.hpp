#pragma once
/**
 * @file Resources.hpp
 * @brief RAII-владельцы ресурсов RHI: Texture, RenderTarget, Mesh, Pipeline — одинаковы для OpenGL и Vulkan.
 *
 * Ручка (RHI::TextureId…) — просто число; объект этого файла владеет ресурсом и освобождает его
 * в деструкторе. Объекты только перемещаются и должны умереть раньше устройства.
 */

#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Geometry3D.hpp>
#include <RendererSystem/Core/Image.hpp>
#include <RendererSystem/Mesh/MeshData.hpp>
#include <RendererSystem/RHI/Device.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace RendererSystem {

/**
 * @brief Текстура RGBA8 в видеопамяти.
 *
 * UV (0, 0) — левый верхний пиксель Image (строка 0 — верх) на любом бэкенде.
 */
class Texture {
public:
    /**
     * @brief Загружает изображение.
     * @throws RendererError Если изображение пустое.
     */
    [[nodiscard]] static Texture create(RHI::Device& device, const Image& image, const TextureDesc& desc = {});
    /// @brief Пустая текстура заданного размера.
    [[nodiscard]] static Texture create_empty(RHI::Device& device, int width, int height, const TextureDesc& desc = {});

    Texture() = default; ///< Пустая (ZII): рисуется белой.
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    /**
     * @brief Заменяет пиксели (размер должен совпадать).
     * @throws RendererError Если размер отличается.
     */
    void update(const Image& image);
    /// @brief Перестраивает mip-уровни (после рисования в текстуру).
    void generate_mipmaps() const;

    [[nodiscard]] RHI::TextureId id() const noexcept { return m_id; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] const TextureDesc& desc() const noexcept { return m_desc; }
    [[nodiscard]] bool valid() const noexcept { return m_id.valid(); }

private:
    friend class RenderTarget;
    void release() noexcept;

    RHI::Device* m_device = nullptr;
    RHI::TextureId m_id{};
    int m_width = 0;
    int m_height = 0;
    TextureDesc m_desc{};
    bool m_owned = true; ///< false — текстура цели (ею владеет RenderTarget).
};

/**
 * @brief Цель рендера: рисуем в неё, потом используем color() как обычную текстуру.
 *
 * Применения: лица карт, превью, миникарта, пост-обработка, тесты (read_pixels).
 * Рисуйте текстуру цели с UV из uv() — тогда кадр не перевёрнут ни в OpenGL, ни в Vulkan.
 */
class RenderTarget {
public:
    /// @brief Создаёт цель; ошибка — текстом.
    [[nodiscard]] static std::expected<RenderTarget, std::string> create(RHI::Device& device, int width, int height,
                                                                         const TargetDesc& desc = {});

    RenderTarget() = default;
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    RenderTarget(RenderTarget&& other) noexcept;
    RenderTarget& operator=(RenderTarget&& other) noexcept;

    /// @brief Рисовать сюда (область вывода — вся цель).
    void bind() const;
    /// @brief Рисовать на экран.
    static void bind_screen(RHI::Device& device);

    [[nodiscard]] const Texture& color() const noexcept { return m_color; }
    [[nodiscard]] int width() const noexcept { return m_color.width(); }
    [[nodiscard]] int height() const noexcept { return m_color.height(); }
    [[nodiscard]] bool has_depth() const noexcept { return m_depth; }
    [[nodiscard]] RHI::TargetId id() const noexcept { return m_id; }
    /// @brief UV, при которой color() выглядит так, как была нарисована (зависит от бэкенда).
    [[nodiscard]] UvRect uv() const noexcept;
    /// @brief Перестраивает mip-уровни цветовой текстуры после рисования.
    void update_mipmaps() const { m_color.generate_mipmaps(); }
    /// @brief Пиксели (строка 0 — верх). Синхронизирует CPU и GPU — не в горячем пути.
    [[nodiscard]] Image read_pixels() const;

private:
    void release() noexcept;

    RHI::Device* m_device = nullptr;
    RHI::TargetId m_id{};
    Texture m_color{};
    bool m_depth = false;
};

/**
 * @brief Сетка в видеопамяти: буфер вершин (любая раскладка) и необязательный буфер индексов.
 */
class Mesh {
public:
    /// @brief Пустая сетка с раскладкой `layout`.
    [[nodiscard]] static Mesh create(RHI::Device& device, const RHI::VertexLayout& layout,
                                     RHI::BufferUsage usage = RHI::BufferUsage::Static);
    /// @brief Сетка из MeshData (раскладка Vertex3D).
    [[nodiscard]] static Mesh create(RHI::Device& device, const MeshData& data, RHI::BufferUsage usage = RHI::BufferUsage::Static);

    Mesh() = default; ///< Пустая (ZII): рисуется как «ничего».
    ~Mesh();
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    /**
     * @brief Загружает вершины (байты в раскладке сетки) и необязательные индексы.
     * @throws RendererError Размер не кратен шагу раскладки или сетка не создана.
     */
    void upload_bytes(std::span<const std::byte> vertices, std::span<const std::uint32_t> indices = {});
    /// @brief Загружает массив вершин любого типа.
    template<typename Vertex>
    void upload(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices = {}) {
        upload_bytes(std::as_bytes(vertices), indices);
    }
    /// @brief Загружает MeshData (раскладка должна быть Vertex3D) и запоминает границы.
    void upload(const MeshData& data);

    /// @brief Вызов рисования всей сетки конвейером `pipeline` (дальше — заполнить текстуру и uniform).
    [[nodiscard]] RHI::DrawCall draw_call(RHI::PipelineId pipeline) const noexcept;

    [[nodiscard]] std::size_t vertex_count() const noexcept { return m_vertex_count; }
    [[nodiscard]] std::size_t index_count() const noexcept { return m_index_count; }
    [[nodiscard]] std::size_t gpu_bytes() const noexcept {
        return m_vertex_count * m_layout.stride + m_index_count * sizeof(std::uint32_t);
    }
    [[nodiscard]] const Aabb& bounds() const noexcept { return m_bounds; }
    void set_bounds(const Aabb& bounds) noexcept { m_bounds = bounds; }
    [[nodiscard]] const RHI::VertexLayout& layout() const noexcept { return m_layout; }
    [[nodiscard]] bool valid() const noexcept { return m_vertices.valid(); }

private:
    void release() noexcept;

    RHI::Device* m_device = nullptr;
    RHI::VertexLayout m_layout{};
    RHI::BufferId m_vertices{};
    RHI::BufferId m_indices{};
    std::size_t m_vertex_count = 0;
    std::size_t m_index_count = 0;
    Aabb m_bounds{};
};

/**
 * @brief Конвейер (шейдеры + состояние) — для своих шейдеров игры (воксели, эффекты).
 */
class Pipeline {
public:
    [[nodiscard]] static std::expected<Pipeline, std::string> create(RHI::Device& device, const RHI::PipelineDesc& desc);

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other) noexcept;

    [[nodiscard]] RHI::PipelineId id() const noexcept { return m_id; }
    [[nodiscard]] bool valid() const noexcept { return m_id.valid(); }

private:
    void release() noexcept;

    RHI::Device* m_device = nullptr;
    RHI::PipelineId m_id{};
};

} // namespace RendererSystem
