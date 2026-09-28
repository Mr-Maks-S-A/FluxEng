/**
 * @example 02_offscreen_png.cpp
 * Рендер без видимого окна: кадр рисуется в Framebuffer и сохраняется в PNG.
 * Так делаются превью (иконки заклинаний, миникарта) и скриншоты в тестах.
 *
 * Результат: `renderer_offscreen.png` в текущем каталоге (путь можно передать аргументом).
 */

#include <RendererSystem/RendererSystem.hpp>
#include <WindowSystem/Window.hpp>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

#include <numbers>
#include <print>
#include <string>

using namespace RendererSystem;

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "renderer_offscreen.png";

    // Окно нужно только ради OpenGL-контекста, поэтому оно скрыто.
    Window window(16, 16, "offscreen", false);
    if (window.getNativeWindow() == nullptr) {
        std::println(stderr, "cannot create an OpenGL context");
        return 1;
    }

    auto renderer = Renderer2D::create();
    auto target = GL::Framebuffer::create(256, 256);
    if (!renderer || !target) {
        std::println(stderr, "{}", !renderer ? renderer.error() : target.error());
        return 1;
    }

    const TextureHandle checker = renderer->create_texture(Image::checkerboard(8, 8, 2, Colors::white, Colors::magenta));

    // Камера: мир [0, 256) × [0, 256), пиксель в пиксель.
    const Camera2D camera{.position = {128.0f, 128.0f}, .viewport = {256.0f, 256.0f}};

    target->bind();
    renderer->clear(Color::from_rgba(0x202028FF));
    renderer->begin(camera);
    renderer->draw(SpriteInstance{.position = {128.0f, 128.0f}, .size = {160.0f, 160.0f},
                                  .rotation = std::numbers::pi_v<float> / 8, .texture = checker});
    renderer->fill_rect({{16.0f, 16.0f}, {48.0f, 48.0f}}, Colors::red.with_alpha(200), 1);
    renderer->draw_rect({{8.0f, 8.0f}, {240.0f, 240.0f}}, 3.0f, Colors::yellow, 2);
    renderer->draw_line({16.0f, 240.0f}, {240.0f, 16.0f}, 2.0f, Colors::green, 2);
    const RenderStats stats = renderer->end();
    GL::Framebuffer::bind_default();

    const Image pixels = target->read_pixels();
    if (stbi_write_png(output.c_str(), pixels.width(), pixels.height(), 4, pixels.pixels().data(),
                       pixels.width() * 4) == 0) {
        std::println(stderr, "cannot write '{}'", output);
        return 1;
    }
    std::println("saved {} ({} quads, {} draw calls)", output, stats.quads, stats.draw_calls);
    return 0;
}
