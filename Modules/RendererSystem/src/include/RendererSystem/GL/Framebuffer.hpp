#pragma once
/**
 * @file Framebuffer.hpp
 * @brief Внеэкранная цель рендера (FBO с цветовой текстурой).
 */

#include <RendererSystem/Core/Image.hpp>
#include <RendererSystem/GL/Texture.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace RendererSystem::GL {

/**
 * @brief Рендер в текстуру.
 *
 * Применения: миникарта, превью заклинаний и инструментов в UI, пост-обработка,
 * пиксель-арт в низком разрешении с последующим масштабированием, а также
 * тесты, которые рисуют кадр и проверяют пиксели.
 *
 * @pre Все методы требуют текущий OpenGL-контекст.
 */
class Framebuffer {
public:
    /**
     * @brief Создаёт FBO с цветовой текстурой RGBA8.
     * @return Framebuffer или текст ошибки (статус неполноты FBO).
     */
    [[nodiscard]] static std::expected<Framebuffer, std::string> create(int width, int height,
                                                                        const TextureDesc& desc = {});

    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer(Framebuffer&& other) noexcept;
    Framebuffer& operator=(Framebuffer&& other) noexcept;

    /// @brief Делает FBO текущей целью и выставляет viewport на весь его размер.
    void bind() const noexcept;
    /// @brief Возвращает вывод в окно (FBO 0). Viewport не меняется.
    static void bind_default() noexcept;

    /// @brief Цветовая текстура — её можно рисовать как обычный спрайт.
    [[nodiscard]] const Texture& color() const noexcept { return m_color; }
    /// @brief Ширина, пиксели.
    [[nodiscard]] int width() const noexcept { return m_color.width(); }
    /// @brief Высота, пиксели.
    [[nodiscard]] int height() const noexcept { return m_color.height(); }

    /**
     * @brief Читает пиксели в Image (строка 0 — верх, как на экране).
     * @note Синхронизирует CPU и GPU — не вызывайте каждый кадр в горячем пути.
     */
    [[nodiscard]] Image read_pixels() const;

private:
    Framebuffer(std::uint32_t id, Texture color) noexcept : m_id(id), m_color(std::move(color)) {}

    std::uint32_t m_id = 0;
    Texture m_color;
};

} // namespace RendererSystem::GL
