/**
 * @file test_render.cpp
 * @brief GPU-тесты на каждом бэкенде RHI (OpenGL, Vulkan): конвейеры, текстуры, цели, Renderer2D, Renderer3D, текст.
 *
 * Каждый тест рисует в RenderTarget и читает пиксели обратно — проверяется весь путь
 * (батч → буферы → шейдер → растеризация → чтение). Тест «одинаковый кадр» сравнивает бэкенды между собой.
 */

#include "RenderDevices.hpp"

#include <RendererSystem/RendererSystem.hpp>

#include <doctest/doctest.h>

#include <glm/ext/matrix_transform.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

using namespace RendererSystem;
using RendererTests::for_each_backend;
using RendererTests::Frame;

namespace {

constexpr int target_size = 64;

/// Камера, показывающая мир [0, 64) × [0, 64) пиксель в пиксель, Y вниз.
Camera2D pixel_camera() {
    return Camera2D{.position = {32.0f, 32.0f}, .zoom = 1.0f, .rotation = 0.0f, .viewport = {64.0f, 64.0f}};
}

Renderer2D make_renderer(RHI::Device& device, const RendererConfig& config = {}) {
    auto renderer = Renderer2D::create(device, config);
    REQUIRE_MESSAGE(renderer.has_value(), renderer.error());
    return std::move(*renderer);
}

RenderTarget make_target(RHI::Device& device, bool depth = false) {
    auto target = RenderTarget::create(device, target_size, target_size, {.depth = depth});
    REQUIRE_MESSAGE(target.has_value(), target.error());
    return std::move(*target);
}

/// Рисует кадр в `target`: `draw` заполняет кадр между begin() и end().
template<typename Draw>
Image render(Renderer2D& renderer, const RenderTarget& target, Draw&& draw) {
    Frame frame(renderer.device());
    target.bind();
    renderer.clear(Colors::black);
    renderer.begin(pixel_camera());
    draw(renderer);
    renderer.end();
    return target.read_pixels();
}

constexpr std::string_view solid_vertex = R"(
FLUX_LOCATION(0) in vec2 a_position;
FLUX_UNIFORM(1, 0) Draw { vec4 color; } draw;
FLUX_VARYING(0) out vec4 v_color;
void main() { v_color = draw.color; FLUX_POSITION(vec4(a_position, 0.0, 1.0)); }
)";
constexpr std::string_view solid_fragment = R"(
FLUX_VARYING(0) in vec4 v_color;
FLUX_LOCATION(0) out vec4 frag_color;
void main() { frag_color = v_color; }
)";

} // namespace

TEST_SUITE("RHI::Device") {
    TEST_CASE("backends report themselves") {
        CHECK(to_string(Backend::OpenGL) == "opengl");
        CHECK(parse_backend("vk") == Backend::Vulkan);
        CHECK_FALSE(parse_backend("dx12").has_value());
        CHECK(backend_compiled(Backend::OpenGL));
        for_each_backend([](RHI::Device& device) {
            CHECK_FALSE(device.info().device_name.empty());
            CHECK_FALSE(device.info().api_version.empty());
        });
    }

    TEST_CASE("custom pipeline with a user vertex layout and per-draw uniforms") {
        for_each_backend([](RHI::Device& device) {
            auto pipeline = Pipeline::create(device, {.name = "solid",
                                                      .shader = {solid_vertex, solid_fragment},
                                                      .layout = RHI::VertexLayout::make(sizeof(glm::vec2), {{0, 2, RHI::AttributeType::Float, 0}})});
            REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error());
            Mesh mesh = Mesh::create(device, RHI::VertexLayout::make(sizeof(glm::vec2), {{0, 2, RHI::AttributeType::Float, 0}}));
            const glm::vec2 triangle[] = {{-1, -1}, {3, -1}, {-1, 3}}; // накрывает весь кадр
            mesh.upload(std::span<const glm::vec2>(triangle));
            CHECK(mesh.vertex_count() == 3);
            RenderTarget target = make_target(device);

            Frame frame(device);
            target.bind();
            device.clear(Colors::black, false);
            const glm::vec4 green{0.0f, 1.0f, 0.0f, 1.0f};
            RHI::DrawCall call = mesh.draw_call(pipeline->id());
            call.draw_uniforms = std::as_bytes(std::span<const glm::vec4>(&green, 1));
            device.draw(call);
            const Image pixels = target.read_pixels();
            CHECK(pixels.pixel(8, 8) == Colors::green);
            CHECK(pixels.pixel(60, 60) == Colors::green);
        });
    }

    TEST_CASE("shader errors are data, with the compiler log") {
        for_each_backend([](RHI::Device& device) {
            auto pipeline = Pipeline::create(device, {.name = "broken", .shader = {solid_vertex, "void main() { this is not glsl; }"},
                                                      .layout = RHI::VertexLayout::make(8, {{0, 2, RHI::AttributeType::Float, 0}})});
            REQUIRE_FALSE(pipeline.has_value());
            CHECK(pipeline.error().find("broken") != std::string::npos);
            CHECK(pipeline.error().find("fragment") != std::string::npos);
        });
    }

    TEST_CASE("a buffer updated after it was drawn keeps the earlier draw intact (orphaning)") {
        for_each_backend([](RHI::Device& device) {
            auto pipeline = Pipeline::create(device, {.name = "solid", .shader = {solid_vertex, solid_fragment},
                                                      .layout = RHI::VertexLayout::make(8, {{0, 2, RHI::AttributeType::Float, 0}})});
            REQUIRE(pipeline.has_value());
            Mesh mesh = Mesh::create(device, RHI::VertexLayout::make(8, {{0, 2, RHI::AttributeType::Float, 0}}), RHI::BufferUsage::Dynamic);
            RenderTarget target = make_target(device);
            const glm::vec2 left[] = {{-1, -1}, {0, -1}, {0, 1}, {-1, -1}, {0, 1}, {-1, 1}};
            const glm::vec2 right[] = {{0, -1}, {1, -1}, {1, 1}, {0, -1}, {1, 1}, {0, 1}};
            const glm::vec4 red{1, 0, 0, 1};
            const glm::vec4 blue{0, 0, 1, 1};

            Frame frame(device);
            target.bind();
            device.clear(Colors::black, false);
            mesh.upload(std::span<const glm::vec2>(left));
            RHI::DrawCall call = mesh.draw_call(pipeline->id());
            call.draw_uniforms = std::as_bytes(std::span<const glm::vec4>(&red, 1));
            device.draw(call);
            mesh.upload(std::span<const glm::vec2>(right)); // тот же буфер, другая геометрия — в том же кадре
            call = mesh.draw_call(pipeline->id());
            call.draw_uniforms = std::as_bytes(std::span<const glm::vec4>(&blue, 1));
            device.draw(call);
            const Image pixels = target.read_pixels();
            CHECK(pixels.pixel(16, 32) == Colors::red);
            CHECK(pixels.pixel(48, 32) == Colors::blue);
        });
    }
}

TEST_SUITE("Texture and RenderTarget") {
    TEST_CASE("create from image, update, reject wrong size") {
        for_each_backend([](RHI::Device& device) {
            const Image image = Image::checkerboard(4, 2, 1, Colors::red, Colors::blue);
            Texture texture = Texture::create(device, image, {.filter = TextureFilter::Linear});
            CHECK(texture.valid());
            CHECK(texture.width() == 4);
            CHECK(texture.height() == 2);
            CHECK(texture.desc().filter == TextureFilter::Linear);
            CHECK_NOTHROW(texture.update(Image(4, 2, Colors::green)));
            CHECK_THROWS_AS(texture.update(Image(2, 2)), RendererError);
            CHECK_THROWS_AS((void)Texture::create(device, Image{}), RendererError);
            Texture mipmapped = Texture::create(device, Image(8, 8, Colors::white), {.mipmaps = true});
            CHECK(mipmapped.valid());
            CHECK_NOTHROW(mipmapped.generate_mipmaps());
        });
    }

    TEST_CASE("clear and read back; invalid size is an error") {
        for_each_backend([](RHI::Device& device) {
            RenderTarget target = make_target(device);
            CHECK(target.width() == target_size);
            {
                Frame frame(device);
                target.bind();
                device.clear(Colors::green, false);
            }
            const Image pixels = target.read_pixels();
            CHECK(pixels.pixel(0, 0) == Colors::green);
            CHECK(pixels.pixel(63, 63) == Colors::green);
            CHECK_FALSE(RenderTarget::create(device, 0, 16).has_value());
        });
    }

    TEST_CASE("a render target drawn as a sprite with uv() is not upside down") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            RenderTarget source = make_target(device);
            RenderTarget result = make_target(device);
            render(renderer, source, [](Renderer2D& r) { r.fill_rect({{0, 0}, {64, 16}}, Colors::red); }); // полоса сверху
            Frame frame(device);
            result.bind();
            renderer.clear(Colors::black);
            renderer.begin(pixel_camera());
            // Текстура цели напрямую в Renderer2D не регистрируется, поэтому проверяем через Renderer3D-квадрат.
            renderer.end();
            auto r3 = Renderer3D::create(device);
            REQUIRE(r3.has_value());
            Camera3D camera{.position = {0, 0, 5}, .viewport = {64, 64}};
            camera.fov_y = 2.0f * std::atan(0.5f / 5.0f);
            r3->begin(camera);
            r3->draw_shape(Renderer3D::Shape::Quad, glm::mat4{1.0f}, {.texture = &source.color(), .uv = source.uv(), .lit = false});
            r3->end();
            const Image pixels = result.read_pixels();
            CHECK(pixels.pixel(32, 4) == Colors::red);
            CHECK(pixels.pixel(32, 60) == Colors::black);
        });
    }
}

TEST_SUITE("Renderer2D") {
    TEST_CASE("fill_rect lands on the expected pixels (Y down)") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            const RenderTarget target = make_target(device);
            const Image pixels = render(renderer, target, [](Renderer2D& r) { r.fill_rect({{0.0f, 0.0f}, {32.0f, 32.0f}}, Colors::red); });
            CHECK(pixels.pixel(8, 8) == Colors::red);
            CHECK(pixels.pixel(31, 31) == Colors::red);
            CHECK(pixels.pixel(40, 8) == Colors::black);
            CHECK(pixels.pixel(8, 40) == Colors::black);
        });
    }

    TEST_CASE("textured sprite keeps image orientation: top-left texel at top-left") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            const RenderTarget target = make_target(device);
            Image image(2, 2);
            image.set_pixel(0, 0, Colors::red);
            image.set_pixel(1, 0, Colors::green);
            image.set_pixel(0, 1, Colors::blue);
            image.set_pixel(1, 1, Colors::yellow);
            const TextureHandle texture = renderer.create_texture(image);
            const Image pixels = render(renderer, target, [&](Renderer2D& r) {
                r.draw(SpriteInstance{.position = {0.0f, 0.0f}, .size = {64.0f, 64.0f}, .pivot = {0.0f, 0.0f}, .texture = texture});
            });
            CHECK(pixels.pixel(16, 16) == Colors::red);
            CHECK(pixels.pixel(48, 16) == Colors::green);
            CHECK(pixels.pixel(16, 48) == Colors::blue);
            CHECK(pixels.pixel(48, 48) == Colors::yellow);
        });
    }

    TEST_CASE("flip X, layers, tint and alpha blending, lines and outlines") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            const RenderTarget target = make_target(device);
            Image image(2, 1);
            image.set_pixel(0, 0, Colors::red);
            image.set_pixel(1, 0, Colors::blue);
            const TextureHandle texture = renderer.create_texture(image);

            Image pixels = render(renderer, target, [&](Renderer2D& r) {
                r.draw(SpriteInstance{.position = {0, 0}, .size = {64, 64}, .pivot = {0, 0}, .texture = texture, .flip = SpriteFlip::X});
            });
            CHECK(pixels.pixel(8, 32) == Colors::blue);
            CHECK(pixels.pixel(56, 32) == Colors::red);

            pixels = render(renderer, target, [](Renderer2D& r) {
                r.fill_rect({{0, 0}, {64, 64}}, Colors::blue, 10);
                r.fill_rect({{0, 0}, {64, 64}}, Colors::red, 0);
            });
            CHECK(pixels.pixel(32, 32) == Colors::blue);

            pixels = render(renderer, target, [](Renderer2D& r) {
                r.fill_rect({{0, 0}, {64, 64}}, Colors::white);
                r.fill_rect({{0, 0}, {64, 64}}, Color{0, 0, 0, 128});
            });
            CHECK(pixels.pixel(32, 32).r == doctest::Approx(127).epsilon(0.02));

            pixels = render(renderer, target, [](Renderer2D& r) {
                r.draw_line({0, 10}, {64, 10}, 4, Colors::green);
                r.draw_rect({{20, 20}, {30, 30}}, 2, Colors::red);
            });
            CHECK(pixels.pixel(32, 10) == Colors::green);
            CHECK(pixels.pixel(32, 20) == Colors::red);
            CHECK(pixels.pixel(35, 35) == Colors::black);
        });
    }

    TEST_CASE("stats, growing buffers, several passes per frame") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device, {.initial_quad_capacity = 4});
            const RenderTarget a = make_target(device);
            const RenderTarget b = make_target(device);
            const TextureHandle red = renderer.create_texture(Image(1, 1, Colors::red));
            const TextureHandle blue = renderer.create_texture(Image(1, 1, Colors::blue));

            Frame frame(device);
            a.bind();
            renderer.clear(Colors::black);
            renderer.begin(pixel_camera());
            for (int i = 0; i < 100; ++i) {
                renderer.draw(SpriteInstance{.position = {static_cast<float>(i % 64), 0.0f}, .texture = i % 2 ? red : blue});
            }
            const RenderStats stats = renderer.end();
            CHECK(stats.quads == 100);
            CHECK(stats.draw_calls == 2);
            CHECK(stats.texture_binds == 2);

            // Второй проход того же кадра в другую цель тем же рендером (тот же буфер вершин, другие данные).
            b.bind();
            renderer.clear(Colors::black);
            renderer.begin(pixel_camera());
            for (int y = 0; y < 64; ++y) renderer.fill_rect({{0.0f, static_cast<float>(y)}, {64.0f, 1.0f}}, Colors::magenta);
            renderer.end();
            CHECK(renderer.last_stats().quads == 64);
            CHECK(b.read_pixels().pixel(5, 63) == Colors::magenta);
            CHECK(a.read_pixels().pixel(1, 0) != Colors::magenta); // первый проход не испорчен вторым
        });
    }

    TEST_CASE("a frame bigger than one ring chunk (10+ MiB of vertices) still draws") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            const RenderTarget target = make_target(device);
            const Image pixels = render(renderer, target, [](Renderer2D& r) {
                for (int i = 0; i < 140'000; ++i) r.fill_rect({{static_cast<float>(i % 64), 0.0f}, {1.0f, 64.0f}}, Colors::blue);
                r.fill_rect({{0, 0}, {64, 64}}, Colors::green, 1);
            });
            CHECK(renderer.last_stats().quads == 140'001);
            CHECK(pixels.pixel(32, 32) == Colors::green);
        });
    }

    TEST_CASE("errors, misuse and moves") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            renderer.begin(pixel_camera());
            CHECK(renderer.end().draw_calls == 0);
            CHECK(renderer.texture_count() == 1);
            CHECK(renderer.texture(TextureHandle::white()).width() == 1);
            CHECK_THROWS_AS((void)renderer.texture(TextureHandle{42}), RendererError);
            renderer.begin(pixel_camera());
            renderer.draw(SpriteInstance{.texture = TextureHandle{42}});
            CHECK_THROWS_AS(renderer.end(), RendererError);
            CHECK_FALSE(renderer.load_texture("missing.png").has_value());
            CHECK_THROWS_AS(renderer.end(), RendererError);
            renderer.begin(pixel_camera());
            CHECK_THROWS_AS(renderer.begin(pixel_camera()), RendererError);
            renderer.end();

            const TextureHandle texture = renderer.create_texture(Image(1, 1, Colors::green));
            Renderer2D moved = std::move(renderer);
            const RenderTarget target = make_target(device);
            const Image pixels = render(moved, target, [&](Renderer2D& r) {
                r.draw(SpriteInstance{.position = {0, 0}, .size = {64, 64}, .pivot = {0, 0}, .texture = texture});
            });
            CHECK(pixels.pixel(32, 32) == Colors::green);
        });
    }

    TEST_CASE("animated sprite frame selects the right sheet cell") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            const RenderTarget target = make_target(device);
            Image sheet(4, 1);
            const Color cells[] = {Colors::red, Colors::green, Colors::blue, Colors::yellow};
            for (int i = 0; i < 4; ++i) sheet.set_pixel(i, 0, cells[i]);
            const TextureHandle texture = renderer.create_texture(sheet);
            AnimationLibrary library;
            const ClipId clip = library.add(AnimationClip{.name = "cycle", .frames = make_grid_frames({.columns = 4, .rows = 1}, 0, 4, 0.1f), .looping = true});
            std::vector<AnimationState> states{AnimationState::start(clip)};
            advance_animations(states, library, 0.25f);
            const Image pixels = render(renderer, target, [&](Renderer2D& r) {
                r.draw(SpriteInstance{.position = {0, 0}, .size = {64, 64}, .pivot = {0, 0}, .uv = current_uv(states[0], library), .texture = texture});
            });
            CHECK(pixels.pixel(32, 32) == Colors::blue);
        });
    }

    TEST_CASE("draw_text puts ink where the text is and returns the block size") {
        for_each_backend([](RHI::Device& device) {
            auto renderer = make_renderer(device);
            const RenderTarget target = make_target(device);
            const FontHandle font = renderer.add_font(Font::builtin(2));
            CHECK(font.valid());
            CHECK_THROWS_AS((void)renderer.font(FontHandle{}), RendererError);
            glm::vec2 size{0.0f};
            const Image pixels = render(renderer, target, [&](Renderer2D& r) {
                size = r.draw_text(font, "TT", {2.0f, 2.0f}, {.color = Colors::white, .shadow = Colors::red});
            });
            CHECK(size == renderer.measure_text(font, "TT"));
            int ink = 0, below = 0;
            for (int y = 0; y < target_size; ++y)
                for (int x = 0; x < target_size; ++x)
                    if (pixels.pixel(x, y) == Colors::white) (y < 2 + static_cast<int>(size.y) ? ink : below) += 1;
            CHECK(ink > 20);
            CHECK(below == 0);
        });
    }
}

TEST_SUITE("Mesh") {
    TEST_CASE("upload counts, bounds, reupload and moves; layout errors") {
        for_each_backend([](RHI::Device& device) {
            Mesh mesh = Mesh::create(device, MeshData::box(), RHI::BufferUsage::Dynamic);
            CHECK(mesh.valid());
            CHECK(mesh.vertex_count() == 24);
            CHECK(mesh.index_count() == 36);
            CHECK(mesh.bounds() == Aabb{glm::vec3{-0.5f}, glm::vec3{0.5f}});
            mesh.upload(MeshData::quad());
            CHECK(mesh.vertex_count() == 4);
            Mesh moved = std::move(mesh);
            CHECK(moved.valid());
            CHECK_FALSE(mesh.valid()); // NOLINT(bugprone-use-after-move)
            CHECK_THROWS_AS((void)Mesh::create(device, RHI::VertexLayout{}), RendererError);
            Mesh raw = Mesh::create(device, vertex3d_layout());
            const std::byte odd[5]{};
            CHECK_THROWS_AS(raw.upload_bytes(odd), RendererError);
            CHECK_THROWS_AS(Mesh{}.upload_bytes({}), RendererError);
        });
    }
}

TEST_SUITE("Renderer3D") {
    TEST_CASE("frame lifecycle is checked") {
        for_each_backend([](RHI::Device& device) {
            auto r = Renderer3D::create(device);
            REQUIRE_MESSAGE(r.has_value(), r.error());
            CHECK_THROWS_AS(r->end(), RendererError);
            CHECK_THROWS_AS(r->draw_shape(Renderer3D::Shape::Cube, glm::mat4{1.0f}), RendererError);
            r->begin(Camera3D{});
            CHECK_THROWS_AS(r->begin(Camera3D{}), RendererError);
            r->end();
        });
    }

    TEST_CASE("depth, culling, transparency and lighting give the same answers on every backend") {
        for_each_backend([](RHI::Device& device) {
            auto r = Renderer3D::create(device);
            REQUIRE(r.has_value());
            RenderTarget target = make_target(device, true);
            const Camera3D camera{.position = {0, 0, 4}, .viewport = {64, 64}};
            Frame frame(device);
            target.bind();
            r->clear(Colors::black);
            r->begin(camera);
            r->draw_shape(Renderer3D::Shape::Quad, glm::translate(glm::mat4{1.0f}, {0, 0, 1}), {.color = Colors::red, .lit = false});   // ближе
            r->draw_shape(Renderer3D::Shape::Quad, glm::scale(glm::mat4{1.0f}, glm::vec3{4.0f}), {.color = Colors::green, .lit = false}); // дальше
            r->draw_shape(Renderer3D::Shape::Cube, glm::translate(glm::mat4{1.0f}, {0, 0, 50}));                                          // за камерой
            r->draw_shape(Renderer3D::Shape::Quad, glm::translate(glm::mat4{1.0f}, {0, 0, 1.5f}) * glm::scale(glm::mat4{1.0f}, glm::vec3{0.3f}),
                          {.color = Color{0, 0, 255, 128}, .lit = false, .blend = BlendMode::Alpha});
            const Render3DStats stats = r->end();
            CHECK(stats.draws == 3);
            CHECK(stats.culled == 1);
            CHECK(stats.transparent == 1);
            const Image pixels = target.read_pixels();
            CHECK(pixels.pixel(26, 26) == Colors::red);       // ближний квадрат (±9 пикселей от центра) закрывает дальний
            CHECK(pixels.pixel(8, 32) == Colors::green);      // дальний (±27 пикселей) виден вокруг
            CHECK(pixels.pixel(1, 1) == Colors::black);       // фон
            const Color center = pixels.pixel(32, 32);        // полупрозрачный синий поверх красного
            CHECK(center.b > 100);
            CHECK(center.r > 100);
        });
    }

    TEST_CASE("back faces are culled with the same winding on every backend") {
        for_each_backend([](RHI::Device& device) {
            auto r = Renderer3D::create(device);
            REQUIRE(r.has_value());
            RenderTarget target = make_target(device, true);
            Frame frame(device);
            target.bind();
            r->clear(Colors::black);
            r->begin(Camera3D{.position = {0, 0, 4}, .viewport = {64, 64}});
            // Квадрат лицом к +Z (к камере) виден; повёрнутый спиной — отсечён.
            r->draw_shape(Renderer3D::Shape::Quad, glm::translate(glm::mat4{1.0f}, {-0.6f, 0, 0}), {.color = Colors::red, .lit = false});
            r->draw_shape(Renderer3D::Shape::Quad, glm::rotate(glm::translate(glm::mat4{1.0f}, {0.6f, 0, 0}), 3.14159265f, glm::vec3{0, 1, 0}),
                          {.color = Colors::blue, .lit = false});
            r->end();
            const Image pixels = target.read_pixels();
            const Camera3D camera{.position = {0, 0, 4}, .viewport = {64, 64}};
            const ScreenPoint front = camera.world_to_screen({-0.6f, 0, 0});
            const ScreenPoint back = camera.world_to_screen({0.6f, 0, 0});
            CHECK(pixels.pixel(static_cast<int>(front.position.x), static_cast<int>(front.position.y)) == Colors::red);
            CHECK(pixels.pixel(static_cast<int>(back.position.x), static_cast<int>(back.position.y)) == Colors::black);
        });
    }
}

TEST_SUITE("Backend parity") {
    TEST_CASE("the same 2D + 3D frame is (almost) pixel-identical on OpenGL and Vulkan") {
        std::map<std::string, Image> frames;
        for (const RendererTests::TestDevice& d : RendererTests::test_devices()) {
            RHI::Device& device = *d.device;
            auto r2 = make_renderer(device);
            auto r3 = Renderer3D::create(device);
            REQUIRE(r3.has_value());
            const FontHandle font = r2.add_font(Font::builtin(1));
            RenderTarget target = make_target(device, true);
            Frame frame(device);
            target.bind();
            r3->clear(Color{20, 30, 40, 255});
            Environment env;
            env.add_point({.position = {1, 1, 2}, .color = Colors::yellow, .intensity = 2, .radius = 5});
            r3->begin(Camera3D{.position = {0, 1, 4}, .viewport = {64, 64}}, env);
            r3->draw_shape(Renderer3D::Shape::Sphere, glm::mat4{1.0f}, {.color = Colors::white});
            r3->draw_shape(Renderer3D::Shape::Cube, glm::translate(glm::mat4{1.0f}, {1.2f, 0, 0}) * glm::scale(glm::mat4{1.0f}, glm::vec3{0.5f}), {.color = Colors::red});
            r3->end();
            r2.begin(pixel_camera());
            r2.fill_rect({{0, 50}, {64, 14}}, Color{0, 0, 0, 160});
            r2.draw_text(font, "GL=VK", {2, 52}, {.color = Colors::yellow});
            r2.end();
            frames.emplace(d.name, target.read_pixels());
        }
        if (frames.size() < 2) {
            MESSAGE("only one backend available — parity not checked");
            return;
        }
        const Image& gl = frames.at("opengl");
        const Image& vk = frames.at("vulkan");
        int different = 0;
        for (std::size_t i = 0; i < gl.pixels().size(); ++i) {
            const Color a = gl.pixels()[i];
            const Color b = vk.pixels()[i];
            if (std::abs(a.r - b.r) > 3 || std::abs(a.g - b.g) > 3 || std::abs(a.b - b.b) > 3) ++different;
        }
        CHECK(different <= 8); // допускаем единичные пиксели на рёбрах (правила растеризации драйверов)
    }
}
