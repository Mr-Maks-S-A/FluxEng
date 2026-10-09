/**
 * @file test_render_pipeline.cpp
 * @brief WindowSystem + RendererSystem на обоих бэкендах RHI: окно движка (OpenGL) и Vulkan без окна.
 *
 * Каждая проверка выполняется на OpenGL (контекст скрытого окна WindowSystem — тот же путь, что у игр)
 * и на Vulkan (устройство без окна со слоями валидации). Результат сравнивается с математикой Camera3D,
 * а в конце — бэкенды друг с другом. Недоступный бэкенд пропускается с сообщением.
 */

#include <RendererSystem/RendererSystem.hpp>
#include <WindowSystem/Window.hpp>

// Контекст OpenGL этому тесту нужен «на руках»: Window платформенных заголовков больше не раскрывает, поэтому GLFW — здесь.
// clang-format off
#include <glad/glad.h>
#include <GLFW/glfw3.h>
// clang-format on

#include <glm/ext/matrix_transform.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace RendererSystem;

namespace {

struct Backend3D {
    std::string name;
    RHI::Device* device = nullptr;
};

/// Скрытое окно с OpenGL-контекстом (одно на процесс, не закрывается — glfwTerminate в статическом деструкторе зависает на Wayland).
WindowSystem::Window* gl_window() {
    static WindowSystem::Window* window = []() -> WindowSystem::Window* {
        auto created = WindowSystem::Window::create({.title = "IntegrationTests", .width = 64, .height = 64, .visible = false, .vsync = false});
        return created ? new WindowSystem::Window(std::move(*created)) : nullptr;
    }();
    return window;
}

/// Устройства обоих бэкендов (живут до конца процесса).
std::vector<Backend3D>& backends() {
    static std::vector<Backend3D> list = [] {
        std::vector<Backend3D> out;
        if (gl_window() != nullptr) {
            if (auto gl = RHI::Device::create({.backend = Backend::OpenGL})) out.push_back({"opengl", gl->release()});
        }
        if (backend_compiled(Backend::Vulkan)) {
            if (auto vk = RHI::Device::create({.backend = Backend::Vulkan, .validation = true})) {
                out.push_back({"vulkan", vk->release()});
            } else {
                MESSAGE("Vulkan is not available: " << vk.error());
            }
        }
        return out;
    }();
    if (gl_window() != nullptr) glfwMakeContextCurrent(static_cast<GLFWwindow*>(gl_window()->native_handle())); // Core::App из соседних тестов мог сменить контекст
    return list;
}

template<typename Fn>
void each_backend(Fn&& fn) {
    if (backends().empty()) {
        MESSAGE("no GPU backend — test skipped");
        return;
    }
    for (const Backend3D& b : backends()) {
        INFO("backend: " << b.name);
        const std::size_t before = b.device->validation_messages();
        b.device->begin_frame(128, 128);
        fn(*b.device);
        b.device->end_frame();
        CHECK_MESSAGE(b.device->validation_messages() == before, "Vulkan validation reported problems");
    }
}

constexpr int size = 128;

RenderTarget target3d(RHI::Device& device) {
    auto fb = RenderTarget::create(device, size, size, {.depth = true});
    REQUIRE_MESSAGE(fb.has_value(), fb.error());
    return std::move(*fb);
}

Renderer3D make3d(RHI::Device& device) {
    auto r = Renderer3D::create(device);
    REQUIRE_MESSAGE(r.has_value(), r.error());
    return std::move(*r);
}

Camera3D front_camera() {
    return Camera3D{.position = {0.0f, 0.0f, 5.0f}, .target = {0.0f, 0.0f, 0.0f}, .viewport = {static_cast<float>(size), static_cast<float>(size)}};
}

Color pixel_at(const Image& image, glm::vec2 p) {
    return image.pixel(static_cast<int>(p.x), static_cast<int>(p.y));
}

glm::mat4 at(glm::vec3 position, glm::vec3 scale) {
    return glm::scale(glm::translate(glm::mat4{1.0f}, position), scale);
}

} // namespace

TEST_SUITE("WindowSystem + RendererSystem (OpenGL and Vulkan)") {
    TEST_CASE("gpu: Renderer3D draws a cube exactly where Camera3D::world_to_screen projects it") {
        each_backend([](RHI::Device& device) {
            RenderTarget fb = target3d(device);
            Renderer3D r = make3d(device);
            const Camera3D camera = front_camera();
            fb.bind();
            r.clear(Colors::black);
            r.begin(camera);
            r.draw_shape(Renderer3D::Shape::Cube, at({0.8f, 0.5f, 0.0f}, glm::vec3{0.6f}), {.color = Colors::red, .lit = false});
            CHECK(r.end().draws == 1);
            const Image image = fb.read_pixels();
            const ScreenPoint cube = camera.world_to_screen({0.8f, 0.5f, 0.0f});
            REQUIRE(cube.visible);
            CHECK(cube.position.x > size * 0.5f);
            CHECK(cube.position.y < size * 0.5f);
            CHECK(pixel_at(image, cube.position) == Colors::red);
            CHECK(pixel_at(image, camera.world_to_screen({-0.8f, -0.5f, 0.0f}).position) == Colors::black);
        });
    }

    TEST_CASE("gpu: depth buffer — the nearer object wins regardless of draw order, culling skips what is behind") {
        each_backend([](RHI::Device& device) {
            RenderTarget fb = target3d(device);
            Renderer3D r = make3d(device);
            const Camera3D camera = front_camera();
            fb.bind();
            r.clear(Colors::black);
            r.begin(camera);
            r.draw_shape(Renderer3D::Shape::Quad, at({0.0f, 0.0f, 1.0f}, glm::vec3{1.0f}), {.color = Colors::red, .lit = false});
            r.draw_shape(Renderer3D::Shape::Quad, at({0.0f, 0.0f, -1.0f}, glm::vec3{3.0f}), {.color = Colors::green, .lit = false});
            r.draw_shape(Renderer3D::Shape::Cube, at({0.0f, 0.0f, 9.0f}, glm::vec3{1.0f}), {.color = Colors::blue, .lit = false});
            const Render3DStats stats = r.end();
            const Image image = fb.read_pixels();
            CHECK(pixel_at(image, {size / 2, size / 2}) == Colors::red);
            CHECK(pixel_at(image, camera.world_to_screen({1.2f, 0.0f, -1.0f}).position) == Colors::green);
            CHECK(stats.culled == 1);
            CHECK(stats.draws == 2);
        });
    }

    TEST_CASE("gpu: lighting — a sphere is brighter on the side facing the sun, a point light adds colour") {
        each_backend([](RHI::Device& device) {
            RenderTarget fb = target3d(device);
            Renderer3D r = make3d(device);
            const Camera3D camera = front_camera();
            Environment env;
            env.ambient = Color{20, 20, 20, 255};
            env.sun = DirectionalLight{.direction = {-1.0f, 0.0f, 0.0f}};
            fb.bind();
            r.clear(Colors::black);
            r.begin(camera, env);
            r.draw_shape(Renderer3D::Shape::Sphere, at({0, 0, 0}, glm::vec3{2.0f}), {.color = Colors::white, .specular = 0.0f});
            r.end();
            const Image lit = fb.read_pixels();
            const Color right = pixel_at(lit, camera.world_to_screen({0.7f, 0.0f, 0.7f}).position);
            const Color left = pixel_at(lit, camera.world_to_screen({-0.7f, 0.0f, 0.7f}).position);
            CHECK(right.r > left.r + 100);

            env.add_point({.position = {-1.5f, 0.0f, 1.5f}, .color = Colors::blue, .intensity = 3.0f, .radius = 4.0f});
            r.clear(Colors::black);
            r.begin(camera, env);
            r.draw_shape(Renderer3D::Shape::Sphere, at({0, 0, 0}, glm::vec3{2.0f}), {.color = Colors::white, .specular = 0.0f});
            r.end();
            const Image with_point = fb.read_pixels();
            const Color left_blue = pixel_at(with_point, camera.world_to_screen({-0.7f, 0.0f, 0.7f}).position);
            CHECK(left_blue.b > left.b + 60);
            CHECK(left_blue.r == left.r);
        });
    }

    TEST_CASE("gpu: text drawn by Renderer2D into a RenderTarget is a texture for a 3D card, not upside down") {
        each_backend([](RHI::Device& device) {
            auto r2 = Renderer2D::create(device);
            REQUIRE(r2.has_value());
            const FontHandle font = r2->add_font(Font::builtin(2));
            auto face = RenderTarget::create(device, 64, 64);
            REQUIRE(face.has_value());
            face->bind();
            r2->clear(Colors::black);
            r2->begin(Camera2D{.position = {32, 32}, .viewport = {64, 64}});
            r2->draw_text(font, "HIHI", {2, 4}, {.color = Colors::white});
            r2->end();
            const Image face_pixels = face->read_pixels();
            int top = 0, bottom = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    if (face_pixels.pixel(x, y).r > 128) (y < 32 ? top : bottom) += 1;
            CHECK(top > 20);
            CHECK(bottom == 0);

            RenderTarget fb = target3d(device);
            Renderer3D r3 = make3d(device);
            Camera3D camera = front_camera();
            camera.fov_y = 2.0f * std::atan(0.5f / 5.0f);
            fb.bind();
            r3.clear(Colors::black);
            r3.begin(camera);
            r3.draw_shape(Renderer3D::Shape::Quad, glm::mat4{1.0f}, {.texture = &face->color(), .uv = face->uv(), .lit = false});
            r3.end();
            const Image card = fb.read_pixels();
            int card_top = 0, card_bottom = 0;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    if (card.pixel(x, y).r > 128) (y < size / 2 ? card_top : card_bottom) += 1;
            CHECK(card_top > 40);
            CHECK(card_bottom == 0);
        });
    }

    TEST_CASE("gpu: a rendered frame survives a PNG round trip (stb_image_write → stb_image)") {
        each_backend([](RHI::Device& device) {
            RenderTarget fb = target3d(device);
            Renderer3D r = make3d(device);
            fb.bind();
            r.clear(Color{10, 20, 30, 255});
            r.begin(front_camera());
            r.draw_shape(Renderer3D::Shape::Sphere, glm::mat4{1.0f}, {.color = Colors::yellow});
            r.end();
            const Image frame = fb.read_pixels();
            const auto decoded = Image::decode(frame.encode_png());
            REQUIRE(decoded.has_value());
            CHECK(std::ranges::equal(decoded->pixels(), frame.pixels()));
        });
    }

    TEST_CASE("gpu: a user pipeline with a voxel-style vertex (position + RGBA8) draws through RHI") {
        each_backend([](RHI::Device& device) {
            struct VoxelVertex {
                float x, y, z;
                Color color;
            };
            const VoxelVertex triangle[] = {{-1, -1, 0, Colors::green}, {3, -1, 0, Colors::green}, {-1, 3, 0, Colors::green}};
            const RHI::VertexLayout layout = RHI::VertexLayout::make(
                sizeof(VoxelVertex), {{0, 3, RHI::AttributeType::Float, 0}, {1, 4, RHI::AttributeType::UnsignedByteNorm, offsetof(VoxelVertex, color)}});
            Mesh mesh = Mesh::create(device, layout);
            mesh.upload(std::span<const VoxelVertex>(triangle));
            auto pipeline = Pipeline::create(device, {.name = "voxel-test",
                                                      .shader = {"FLUX_LOCATION(0) in vec3 p; FLUX_LOCATION(1) in vec4 c; FLUX_VARYING(0) out vec4 v;"
                                                                 " void main(){ v = c; FLUX_POSITION(vec4(p, 1)); }",
                                                                 "FLUX_VARYING(0) in vec4 v; FLUX_LOCATION(0) out vec4 o; void main(){ o = v; }"},
                                                      .layout = layout});
            REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error());
            auto fb = RenderTarget::create(device, 16, 16);
            REQUIRE(fb.has_value());
            fb->bind();
            device.clear(Colors::black, false);
            device.draw(mesh.draw_call(pipeline->id()));
            CHECK(fb->read_pixels().pixel(8, 8) == Colors::green);
        });
    }

    TEST_CASE("gpu: OpenGL and Vulkan render the same lit 3D scene") {
        std::map<std::string, Image> frames;
        each_backend([&](RHI::Device& device) {
            RenderTarget fb = target3d(device);
            Renderer3D r = make3d(device);
            fb.bind();
            r.clear(Color{30, 30, 40, 255});
            Environment env;
            env.add_point({.position = {1.5f, 1.0f, 2.0f}, .color = Colors::yellow, .intensity = 2.0f, .radius = 5.0f});
            env.fog_color = Color{30, 30, 40, 255};
            env.fog_start = 4.0f;
            env.fog_end = 12.0f;
            r.begin(Camera3D{.position = {0, 2, 5}, .viewport = {size, size}}, env);
            r.draw_shape(Renderer3D::Shape::Plane, at({0, -1, 0}, glm::vec3{8.0f}), {.color = Color{90, 140, 90, 255}});
            r.draw_shape(Renderer3D::Shape::Sphere, glm::mat4{1.0f}, {.color = Colors::white, .specular = 0.6f});
            r.draw_shape(Renderer3D::Shape::Cylinder, at({-1.5f, 0, 0}, {0.6f, 1.5f, 0.6f}), {.color = Colors::red});
            r.draw_shape(Renderer3D::Shape::Sphere, at({1.2f, 0, 1}, glm::vec3{0.8f}),
                         {.color = Color{80, 160, 255, 90}, .lit = false, .blend = BlendMode::Additive});
            r.end();
            frames.emplace(device.backend() == Backend::Vulkan ? "vulkan" : "opengl", fb.read_pixels());
        });
        if (frames.size() < 2) {
            MESSAGE("only one backend — parity not checked");
            return;
        }
        const auto a = frames.at("opengl").pixels();
        const auto b = frames.at("vulkan").pixels();
        int different = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::abs(a[i].r - b[i].r) > 4 || std::abs(a[i].g - b[i].g) > 4 || std::abs(a[i].b - b[i].b) > 4) ++different;
        }
        CHECK(different <= 16); // 128×128 = 16 384 пикселей; допускаем отдельные пиксели на рёбрах
    }
}
