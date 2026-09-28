#pragma once
/**
 * @file Renderer2D.hpp
 * @brief Батчевый 2D-рендер на OpenGL 3.3: спрайты, прямоугольники, линии.
 */

#include <RendererSystem/Batch/SpriteBatch.hpp>
#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Handles.hpp>
#include <RendererSystem/Core/Image.hpp>
#include <RendererSystem/GL/Shader.hpp>
#include <RendererSystem/GL/Texture.hpp>
#include <RendererSystem/Scene/Camera2D.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace RendererSystem {

/**
 * @brief Настройки Renderer2D.
 */
struct RendererConfig {
    std::size_t initial_quad_capacity = 4096;     ///< Начальная ёмкость GPU-буферов; растёт автоматически.
    SortMode sort_mode = SortMode::LayerThenTexture; ///< Порядок спрайтов внутри слоя.
};

/**
 * @brief Статистика последнего кадра.
 */
struct RenderStats {
    std::uint32_t quads = 0;         ///< Нарисовано четырёхугольников.
    std::uint32_t draw_calls = 0;    ///< Вызовов glDrawElements.
    std::uint32_t texture_binds = 0; ///< Смен текстуры.
};

/**
 * @brief 2D-рендер: собирает кадр в SpriteBatch и рисует его минимумом draw call'ов.
 *
 * Рендеру нужен только текущий OpenGL-контекст — окно, ввод и время кадра
 * принадлежат другим модулям.
 *
 * Кадр:
 * @code
 * renderer.set_viewport(window.getWidth(), window.getHeight());
 * renderer.clear(Colors::black);
 * renderer.begin(camera);
 * renderer.draw(SpriteInstance{.position = {10, 20}, .size = {16, 16}, .texture = goblin});
 * renderer.fill_rect({{0, 0}, {100, 5}}, Colors::red);
 * RenderStats stats = renderer.end();
 * @endcode
 *
 * @pre Все методы требуют текущий OpenGL 3.3 core контекст.
 * @note Не потокобезопасен (как и сам OpenGL-контекст).
 */
class Renderer2D {
public:
    /**
     * @brief Создаёт рендер в текущем контексте.
     * @return Рендер или текст ошибки (нет контекста, не собрался встроенный шейдер).
     */
    [[nodiscard]] static std::expected<Renderer2D, std::string> create(const RendererConfig& config = {});

    ~Renderer2D();
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;
    Renderer2D(Renderer2D&& other) noexcept;
    Renderer2D& operator=(Renderer2D&& other) noexcept;

    // ================================================================= ресурсы

    /// @brief Загружает изображение в видеопамять и возвращает дескриптор.
    [[nodiscard]] TextureHandle create_texture(const Image& image, const GL::TextureDesc& desc = {});

    /// @brief Загружает изображение из файла.
    [[nodiscard]] std::expected<TextureHandle, std::string> load_texture(const std::filesystem::path& path,
                                                                         const GL::TextureDesc& desc = {});

    /**
     * @brief Текстура по дескриптору.
     * @throws RendererError Дескриптор не из этого рендера.
     */
    [[nodiscard]] const GL::Texture& texture(TextureHandle handle) const;

    /// @brief Количество текстур (включая встроенную белую).
    [[nodiscard]] std::size_t texture_count() const noexcept { return m_textures.size(); }

    // ================================================================= кадр

    /// @brief glViewport на всю область вывода.
    void set_viewport(int width, int height) noexcept;

    /// @brief Очищает текущую цель цветом.
    void clear(Color color) noexcept;

    /// @brief Начинает кадр с матрицей `view_projection`.
    void begin(const glm::mat4& view_projection);
    /// @brief Начинает кадр с камерой.
    void begin(const Camera2D& camera) { begin(camera.view_projection()); }

    /// @brief Спрайт.
    void draw(const SpriteInstance& sprite) { m_batch.submit(sprite); }
    /// @brief Залитый прямоугольник.
    void fill_rect(const Rect& rect, Color color, std::int32_t layer = 0) { m_batch.submit_rect(rect, color, layer); }
    /// @brief Контур прямоугольника.
    void draw_rect(const Rect& rect, float thickness, Color color, std::int32_t layer = 0) {
        m_batch.submit_rect_outline(rect, thickness, color, layer);
    }
    /// @brief Отрезок.
    void draw_line(glm::vec2 from, glm::vec2 to, float thickness, Color color, std::int32_t layer = 0) {
        m_batch.submit_line(from, to, thickness, color, layer);
    }

    /**
     * @brief Завершает кадр: сортирует, загружает вершины и рисует.
     * @return Статистика кадра.
     */
    RenderStats end();

    /// @brief Батч текущего кадра (например чтобы заполнить его напрямую из ECS).
    [[nodiscard]] SpriteBatch& batch() noexcept { return m_batch; }

    /// @brief Статистика последнего end().
    [[nodiscard]] const RenderStats& last_stats() const noexcept { return m_stats; }

private:
    Renderer2D(GL::Shader shader, const RendererConfig& config);

    void ensure_capacity(std::size_t quads);
    void release() noexcept;

    GL::Shader m_shader;
    std::vector<GL::Texture> m_textures;
    SpriteBatch m_batch;
    glm::mat4 m_view_projection{1.0f};
    RenderStats m_stats{};

    std::uint32_t m_vao = 0;
    std::uint32_t m_vbo = 0;
    std::uint32_t m_ebo = 0;
    std::size_t m_quad_capacity = 0;
    bool m_in_frame = false;
};

} // namespace RendererSystem
