/**
 * @example 04_scene_3d.cpp
 * 3D-сцена без видимого окна: стол с процедурной фактурой (stb_perlin), карта со скруглёнными углами,
 * лицо которой нарисовано Renderer2D с текстом (stb_truetype) в Framebuffer, освещение солнцем и
 * точечным светом, прозрачный купол, подпись в 2D-оверлее над объектом (Camera3D::world_to_screen).
 * Кадр сохраняется в PNG (stb_image_write).
 *
 * Результат: `renderer_scene_3d.png` в текущем каталоге (путь можно передать аргументом).
 * `--backend gl|vulkan` — графический API: картинка одна и та же.
 */

#include "ExampleContext.hpp"

#include <glm/ext/matrix_transform.hpp>

#include <print>
#include <string>

using namespace RendererSystem;

int main(int argc, char** argv) {
    const std::string output = argc > 1 && argv[1][0] != '-' ? argv[1] : "renderer_scene_3d.png";
    constexpr int width = 640;
    constexpr int height = 400;

    auto context = Example::open("scene 3d", 16, 16, false, Example::backend_from_args(argc, argv));
    if (!context) {
        std::println(stderr, "cannot create a device: {}", context.error());
        return 1;
    }
    RHI::Device& device = *context->device;
    auto r2 = Renderer2D::create(device);
    auto r3 = Renderer3D::create(device);
    auto frame = RenderTarget::create(device, width, height, {.depth = true});
    auto face = RenderTarget::create(device, 256, 366, {.color = {.filter = TextureFilter::Linear, .mipmaps = true}});
    if (!r2 || !r3 || !frame || !face) {
        std::println(stderr, "renderer setup failed");
        return 1;
    }

    // Шрифт: системный TTF с кириллицей, иначе встроенный.
    auto ttf = Font::load_system({.pixel_height = 40.0f});
    const FontHandle font = r2->add_font(ttf ? std::move(*ttf) : Font::builtin(3));

    // Фактуры из шума.
    const TextureHandle felt = r2->create_texture(
        Procedural::noise_image({.width = 256, .height = 256, .scale = 10.0f, .octaves = 3},
                                {{0.0f, Color::from_rgba(0x123432FF)}, {1.0f, Color::from_rgba(0x24524EFF)}}),
        {.filter = TextureFilter::Linear, .wrap = TextureWrap::ClampToEdge, .mipmaps = true});
    const TextureHandle art = r2->create_texture(
        Procedural::noise_image({.width = 200, .height = 140, .scale = 3.0f, .z = 5.0f, .kind = Procedural::NoiseKind::Turbulence},
                                {{0.0f, Color::from_rgba(0x2A0A00FF)}, {0.6f, Color::from_rgba(0xFF5A1AFF)}, {1.0f, Colors::yellow}}),
        {.filter = TextureFilter::Linear});

    // Лицо карты: Renderer2D → Framebuffer → текстура для 3D.
    device.begin_frame(width, height);
    face->bind();
    r2->clear(Color::from_rgba(0x3B2F5CFF));
    r2->begin(Camera2D{.position = {128, 183}, .viewport = {256, 366}});
    r2->draw(SpriteInstance{.position = {20, 24}, .size = {216, 150}, .pivot = {0, 0}, .texture = art});
    r2->draw_text(font, "Огненный шар", {0, 190}, {.size = 30, .color = Colors::white, .align = TextAlign::Center, .max_width = 256, .layer = 1, .shadow = Colors::black});
    r2->draw_text(font, "Нанесите 6 урона.", {16, 250}, {.size = 24, .color = Color::from_rgba(0xEDE3C8FF), .align = TextAlign::Center, .max_width = 224, .layer = 1});
    r2->end();
    face->update_mipmaps();

    const Mesh card = Mesh::create(device, MeshData::rounded_slab({1.4f, 2.0f}, 0.04f, 0.1f));
    const Mesh card_face = Mesh::create(device, MeshData::rounded_rect({1.4f, 2.0f}, 0.1f));
    const Camera3D camera{.position = {0.0f, 3.2f, 4.2f}, .target = {0.0f, 0.3f, 0.0f}, .viewport = {width, height}};

    Environment env;
    env.sun.direction = {-0.4f, -1.0f, -0.5f};
    env.add_point({.position = {1.6f, 0.8f, 0.8f}, .color = Color{255, 140, 60, 255}, .intensity = 2.0f, .radius = 3.0f});

    frame->bind();
    r3->clear(Color::from_rgba(0x0E0D14FF));
    r3->begin(camera, env);
    r3->draw_shape(Renderer3D::Shape::Plane, glm::scale(glm::mat4{1.0f}, {8.0f, 1.0f, 5.0f}), {.texture = &r2->texture(felt)});
    const glm::mat4 card_model = glm::rotate(glm::translate(glm::mat4{1.0f}, {-0.2f, 1.0f, 0.0f}), -0.9f, glm::vec3{1.0f, 0.0f, 0.0f});
    r3->draw(card, card_model, {.color = Color::from_rgba(0x24306EFF)});
    r3->draw(card_face, glm::translate(card_model, {0.0f, 0.0f, 0.023f}), {.texture = &face->color(), .uv = face->uv()});
    r3->draw_shape(Renderer3D::Shape::Sphere, glm::scale(glm::translate(glm::mat4{1.0f}, {1.6f, 0.4f, 0.8f}), glm::vec3{0.35f}),
                   {.color = Color{255, 160, 60, 255}, .emissive = {1.4f, 0.7f, 0.2f}, .lit = false});
    r3->draw_shape(Renderer3D::Shape::Sphere, glm::scale(glm::translate(glm::mat4{1.0f}, {-1.9f, 0.3f, 0.6f}), {1.2f, 0.6f, 1.2f}),
                   {.color = Color{255, 214, 90, 80}, .emissive = {0.4f, 0.32f, 0.06f}, .lit = false, .blend = BlendMode::Additive});
    const Render3DStats stats3d = r3->end();

    // 2D-оверлей: подпись над сферой там, куда её проецирует камера.
    const ScreenPoint label = camera.world_to_screen({1.6f, 0.9f, 0.8f});
    r2->begin(Camera2D{.position = {width * 0.5f, height * 0.5f}, .viewport = {width, height}});
    r2->draw_text(font, "снаряд", label.position - glm::vec2{60.0f, 20.0f},
                  {.size = 22, .color = Colors::yellow, .align = TextAlign::Center, .max_width = 120, .shadow = Colors::black});
    r2->end();

    const Image pixels = frame->read_pixels();
    device.end_frame();
    if (const auto saved = pixels.save_png(output); !saved) {
        std::println(stderr, "{}", saved.error());
        return 1;
    }
    std::println("saved {} [{}] ({} objects, {} triangles, {} transparent; font: {})", output, to_string(device.backend()), stats3d.draws,
                 stats3d.triangles, stats3d.transparent, r2->font(font).source());
    return 0;
}
