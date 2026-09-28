/**
 * @file test_gl.cpp
 * @brief GPU-тесты: шейдеры, текстуры, framebuffer и реальные пиксели Renderer2D.
 *
 * Каждый тест рисует в Framebuffer и читает пиксели обратно, поэтому проверяется
 * весь путь: батч → вершины → шейдер → растеризация. Без OpenGL тесты пропускаются.
 */

#include "GlTestContext.hpp"

#include <RendererSystem/RendererSystem.hpp>

#include <doctest/doctest.h>

#include <numbers>

using namespace RendererSystem;

namespace {

constexpr int target_size = 64;

/// Камера, показывающая мир [0, 64) × [0, 64) пиксель в пиксель, Y вниз.
Camera2D pixel_camera() {
    return Camera2D{.position = {32.0f, 32.0f}, .zoom = 1.0f, .rotation = 0.0f, .viewport = {64.0f, 64.0f}};
}

Renderer2D make_renderer(const RendererConfig& config = {}) {
    auto renderer = Renderer2D::create(config);
    REQUIRE_MESSAGE(renderer.has_value(), renderer.error());
    return std::move(*renderer);
}

GL::Framebuffer make_target() {
    auto target = GL::Framebuffer::create(target_size, target_size);
    REQUIRE_MESSAGE(target.has_value(), target.error());
    return std::move(*target);
}

/// Рисует кадр в `target`: `draw` заполняет кадр между begin() и end().
template<typename Draw>
Image render(Renderer2D& renderer, const GL::Framebuffer& target, Draw&& draw) {
    target.bind();
    renderer.clear(Colors::black);
    renderer.begin(pixel_camera());
    draw(renderer);
    renderer.end();
    GL::Framebuffer::bind_default();
    return target.read_pixels();
}

} // namespace

TEST_SUITE("GL::Shader") {
    TEST_CASE("valid program compiles and exposes uniforms") {
        REQUIRE_GL_CONTEXT();
        auto shader = GL::Shader::from_source(
            "#version 330 core\nlayout(location=0) in vec2 p; uniform mat4 m; void main(){ gl_Position = m * vec4(p,0,1); }",
            "#version 330 core\nuniform vec4 c; out vec4 o; void main(){ o = c; }");
        REQUIRE_MESSAGE(shader.has_value(), shader.error());
        CHECK(shader->id() != 0);
        CHECK(shader->uniform_location("c") >= 0);
        CHECK(shader->uniform_location("missing") == -1);
        CHECK(shader->uniform_location("c") == shader->uniform_location("c")); // из кэша
    }

    TEST_CASE("compile error returns the stage and driver log") {
        REQUIRE_GL_CONTEXT();
        auto shader = GL::Shader::from_source("#version 330 core\nvoid main(){ gl_Position = vec4(0); }",
                                              "#version 330 core\nvoid main(){ this is not glsl; }");
        REQUIRE_FALSE(shader.has_value());
        CHECK(shader.error().find("fragment shader failed to compile") != std::string::npos);
    }

    TEST_CASE("missing shader files are reported") {
        REQUIRE_GL_CONTEXT();
        auto shader = GL::Shader::from_files("missing.vert", "missing.frag");
        REQUIRE_FALSE(shader.has_value());
        CHECK(shader.error().find("cannot open shader file") != std::string::npos);
    }

    TEST_CASE("move transfers ownership") {
        REQUIRE_GL_CONTEXT();
        auto shader = GL::Shader::from_source("#version 330 core\nvoid main(){ gl_Position = vec4(0); }",
                                              "#version 330 core\nout vec4 o; void main(){ o = vec4(1); }");
        REQUIRE(shader.has_value());
        const auto id = shader->id();
        GL::Shader moved = std::move(*shader);
        CHECK(moved.id() == id);
        CHECK(shader->id() == 0); // NOLINT(bugprone-use-after-move)
    }
}

TEST_SUITE("GL::Texture") {
    TEST_CASE("create from image, update, reject wrong size") {
        REQUIRE_GL_CONTEXT();
        const Image image = Image::checkerboard(4, 2, 1, Colors::red, Colors::blue);
        GL::Texture texture = GL::Texture::create(image, {.filter = GL::TextureFilter::Linear});
        CHECK(texture.id() != 0);
        CHECK(texture.width() == 4);
        CHECK(texture.height() == 2);
        CHECK(texture.desc().filter == GL::TextureFilter::Linear);

        CHECK_NOTHROW(texture.update(Image(4, 2, Colors::green)));
        CHECK_THROWS_AS(texture.update(Image(2, 2)), RendererError);
        CHECK_THROWS_AS((void)GL::Texture::create(Image{}), RendererError);
    }

    TEST_CASE("mipmapped texture") {
        REQUIRE_GL_CONTEXT();
        GL::Texture texture = GL::Texture::create(Image(8, 8, Colors::white), {.mipmaps = true});
        CHECK(texture.id() != 0);
    }
}

TEST_SUITE("GL::Framebuffer") {
    TEST_CASE("create, clear and read back") {
        REQUIRE_GL_CONTEXT();
        GL::Framebuffer target = make_target();
        CHECK(target.width() == target_size);
        target.bind();
        auto renderer = make_renderer();
        renderer.clear(Colors::green);
        GL::Framebuffer::bind_default();

        const Image pixels = target.read_pixels();
        CHECK(pixels.pixel(0, 0) == Colors::green);
        CHECK(pixels.pixel(63, 63) == Colors::green);
    }

    TEST_CASE("invalid size is an error, not a crash") {
        REQUIRE_GL_CONTEXT();
        CHECK_FALSE(GL::Framebuffer::create(0, 16).has_value());
    }
}

TEST_SUITE("Renderer2D") {
    TEST_CASE("fill_rect lands on the expected pixels (Y down)") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        const Image pixels = render(renderer, target, [](Renderer2D& r) {
            r.fill_rect({{0.0f, 0.0f}, {32.0f, 32.0f}}, Colors::red); // левая верхняя четверть
        });
        CHECK(pixels.pixel(8, 8) == Colors::red);
        CHECK(pixels.pixel(31, 31) == Colors::red);
        CHECK(pixels.pixel(40, 8) == Colors::black);
        CHECK(pixels.pixel(8, 40) == Colors::black);
    }

    TEST_CASE("textured sprite keeps image orientation: top-left texel at top-left") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        Image image(2, 2);
        image.set_pixel(0, 0, Colors::red);    // верх-лево
        image.set_pixel(1, 0, Colors::green);  // верх-право
        image.set_pixel(0, 1, Colors::blue);   // низ-лево
        image.set_pixel(1, 1, Colors::yellow); // низ-право
        const TextureHandle texture = renderer.create_texture(image);

        const Image pixels = render(renderer, target, [&](Renderer2D& r) {
            r.draw(SpriteInstance{.position = {0.0f, 0.0f}, .size = {64.0f, 64.0f}, .pivot = {0.0f, 0.0f},
                                  .texture = texture});
        });
        CHECK(pixels.pixel(16, 16) == Colors::red);
        CHECK(pixels.pixel(48, 16) == Colors::green);
        CHECK(pixels.pixel(16, 48) == Colors::blue);
        CHECK(pixels.pixel(48, 48) == Colors::yellow);
    }

    TEST_CASE("flip X mirrors the texture") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        Image image(2, 1);
        image.set_pixel(0, 0, Colors::red);
        image.set_pixel(1, 0, Colors::blue);
        const TextureHandle texture = renderer.create_texture(image);

        const Image pixels = render(renderer, target, [&](Renderer2D& r) {
            r.draw(SpriteInstance{.position = {0.0f, 0.0f}, .size = {64.0f, 64.0f}, .pivot = {0.0f, 0.0f},
                                  .texture = texture, .flip = SpriteFlip::X});
        });
        CHECK(pixels.pixel(8, 32) == Colors::blue);
        CHECK(pixels.pixel(56, 32) == Colors::red);
    }

    TEST_CASE("higher layer is drawn on top regardless of submission order") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        const Image pixels = render(renderer, target, [](Renderer2D& r) {
            r.fill_rect({{0.0f, 0.0f}, {64.0f, 64.0f}}, Colors::blue, 10);
            r.fill_rect({{0.0f, 0.0f}, {64.0f, 64.0f}}, Colors::red, 0);
        });
        CHECK(pixels.pixel(32, 32) == Colors::blue);
    }

    TEST_CASE("color tint and alpha blending") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        const Image pixels = render(renderer, target, [](Renderer2D& r) {
            r.fill_rect({{0.0f, 0.0f}, {64.0f, 64.0f}}, Colors::white);
            r.fill_rect({{0.0f, 0.0f}, {64.0f, 64.0f}}, Color{0, 0, 0, 128}); // полупрозрачный чёрный
        });
        const Color c = pixels.pixel(32, 32);
        CHECK(c.r == doctest::Approx(127).epsilon(0.02));
        CHECK(c.g == c.r);
    }

    TEST_CASE("lines and outlines are rendered") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        const Image pixels = render(renderer, target, [](Renderer2D& r) {
            r.draw_line({0.0f, 10.0f}, {64.0f, 10.0f}, 4.0f, Colors::green);
            r.draw_rect({{20.0f, 20.0f}, {30.0f, 30.0f}}, 2.0f, Colors::red);
        });
        CHECK(pixels.pixel(32, 10) == Colors::green);
        CHECK(pixels.pixel(32, 20) == Colors::red);  // верхняя сторона контура
        CHECK(pixels.pixel(35, 35) == Colors::black); // внутри контура пусто
    }

    TEST_CASE("stats: interleaved textures are merged into one draw call per texture") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();
        const TextureHandle a = renderer.create_texture(Image(1, 1, Colors::red));
        const TextureHandle b = renderer.create_texture(Image(1, 1, Colors::blue));

        target.bind();
        renderer.begin(pixel_camera());
        for (int i = 0; i < 100; ++i) {
            renderer.draw(SpriteInstance{.position = {static_cast<float>(i % 64), 0.0f}, .texture = i % 2 ? a : b});
        }
        const RenderStats stats = renderer.end();
        GL::Framebuffer::bind_default();

        CHECK(stats.quads == 100);
        CHECK(stats.draw_calls == 2);
        CHECK(stats.texture_binds == 2);
        CHECK(renderer.last_stats().draw_calls == 2);
    }

    TEST_CASE("buffers grow beyond the initial capacity") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer({.initial_quad_capacity = 4});
        const GL::Framebuffer target = make_target();

        const Image pixels = render(renderer, target, [](Renderer2D& r) {
            for (int y = 0; y < 64; ++y) {
                r.fill_rect({{0.0f, static_cast<float>(y)}, {64.0f, 1.0f}}, Colors::magenta); // 64 квада
            }
        });
        CHECK(renderer.last_stats().quads == 64);
        CHECK(pixels.pixel(5, 63) == Colors::magenta);
    }

    TEST_CASE("empty frame and resource errors") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        renderer.begin(pixel_camera());
        CHECK(renderer.end().draw_calls == 0);

        CHECK(renderer.texture_count() == 1); // только встроенная белая
        CHECK(renderer.texture(TextureHandle::white()).width() == 1);
        CHECK_THROWS_AS((void)renderer.texture(TextureHandle{42}), RendererError);

        renderer.begin(pixel_camera());
        renderer.draw(SpriteInstance{.texture = TextureHandle{42}});
        CHECK_THROWS_AS(renderer.end(), RendererError);

        const auto missing = renderer.load_texture("missing.png");
        REQUIRE_FALSE(missing.has_value());
    }

    TEST_CASE("begin/end misuse is reported") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        CHECK_THROWS_AS(renderer.end(), RendererError);
        renderer.begin(pixel_camera());
        CHECK_THROWS_AS(renderer.begin(pixel_camera()), RendererError);
        renderer.end();
    }

    TEST_CASE("renderer is movable") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const TextureHandle texture = renderer.create_texture(Image(1, 1, Colors::green));
        Renderer2D moved = std::move(renderer);
        const GL::Framebuffer target = make_target();

        const Image pixels = render(moved, target, [&](Renderer2D& r) {
            r.draw(SpriteInstance{.position = {0.0f, 0.0f}, .size = {64.0f, 64.0f}, .pivot = {0.0f, 0.0f},
                                  .texture = texture});
        });
        CHECK(pixels.pixel(32, 32) == Colors::green);
    }

    TEST_CASE("animated sprite frame selects the right sheet cell") {
        REQUIRE_GL_CONTEXT();
        auto renderer = make_renderer();
        const GL::Framebuffer target = make_target();

        // Спрайт-лист 4×1: красный, зелёный, синий, жёлтый.
        Image sheet(4, 1);
        const Color cells[] = {Colors::red, Colors::green, Colors::blue, Colors::yellow};
        for (int i = 0; i < 4; ++i) {
            sheet.set_pixel(i, 0, cells[i]);
        }
        const TextureHandle texture = renderer.create_texture(sheet);

        AnimationLibrary library;
        const ClipId clip = library.add(AnimationClip{
            .name = "cycle", .frames = make_grid_frames({.columns = 4, .rows = 1}, 0, 4, 0.1f), .looping = true});
        std::vector<AnimationState> states{AnimationState::start(clip)};
        advance_animations(states, library, 0.25f); // кадр 2 — синий

        const Image pixels = render(renderer, target, [&](Renderer2D& r) {
            r.draw(SpriteInstance{.position = {0.0f, 0.0f}, .size = {64.0f, 64.0f}, .pivot = {0.0f, 0.0f},
                                  .uv = current_uv(states[0], library), .texture = texture});
        });
        CHECK(pixels.pixel(32, 32) == Colors::blue);
    }
}
