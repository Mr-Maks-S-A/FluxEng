/**
 * @example 01_window_sprites.cpp
 * Окно движка (WindowSystem) + Renderer2D: тайловый фон, анимированные существа,
 * линии и контуры, камера с панорамой и зумом. ESC — выход.
 *
 * Аргументы: `--frames N` — закрыть окно через N кадров (для автоматического запуска),
 * `--backend gl|vulkan` — графический API.
 */

#include "ExampleContext.hpp"

#include <cmath>
#include <cstdlib>
#include <print>
#include <random>
#include <string_view>
#include <vector>

using namespace RendererSystem;

namespace {

/// Процедурный спрайт-лист 4×1: «существо», которое моргает цветом.
Image make_creature_sheet() {
    Image sheet(64, 16, Colors::transparent);
    const Color body[] = {Color::from_rgba(0x4CAF50FF), Color::from_rgba(0x66BB6AFF), Color::from_rgba(0x81C784FF),
                          Color::from_rgba(0x66BB6AFF)};
    for (int frame = 0; frame < 4; ++frame) {
        const int x = frame * 16;
        sheet.fill_rect(x + 3, 4 + (frame % 2), 10, 10, body[frame]); // тело «подпрыгивает»
        sheet.fill_rect(x + 5, 7, 2, 2, Colors::black);               // глаза
        sheet.fill_rect(x + 9, 7, 2, 2, Colors::black);
    }
    return sheet;
}

struct Creature {
    glm::vec2 position;
    glm::vec2 velocity;
};

int frames_limit(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--frames") {
            return std::atoi(argv[i + 1]);
        }
    }
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    const int max_frames = frames_limit(argc, argv);

    auto opened = Example::open("FluxEng RendererSystem — sprites", 1280, 720, true, Example::backend_from_args(argc, argv));
    if (!opened) {
        std::println(stderr, "cannot create window or device: {}", opened.error());
        return 1;
    }
    WindowSystem::Window& window = opened->window;
    RHI::Device& device = *opened->device;

    auto created = Renderer2D::create(device);
    if (!created) {
        std::println(stderr, "{}", created.error());
        return 1;
    }
    Renderer2D& renderer = *created;

    const TextureHandle ground = renderer.create_texture(
        Image::checkerboard(32, 32, 16, Color::from_rgba(0x3E2F23FF), Color::from_rgba(0x4A3A2CFF)));
    const TextureHandle creature_sheet = renderer.create_texture(make_creature_sheet());

    AnimationLibrary animations;
    const ClipId idle = animations.add(AnimationClip{
        .name = "creature.idle", .frames = make_grid_frames({.columns = 4, .rows = 1}, 0, 4, 0.15f), .looping = true});

    // Данные существ — плоские массивы, как компоненты ECS.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> coord(-600.0f, 600.0f);
    std::uniform_real_distribution<float> speed(-40.0f, 40.0f);
    std::uniform_real_distribution<float> phase(0.0f, 0.6f);
    std::vector<Creature> creatures(500);
    std::vector<AnimationState> animation_states(creatures.size());
    for (std::size_t i = 0; i < creatures.size(); ++i) {
        creatures[i] = {{coord(rng), coord(rng) * 0.6f}, {speed(rng), speed(rng)}};
        animation_states[i] = AnimationState::start(idle);
        animation_states[i].time = phase(rng) * 0.15f; // чтобы не моргали синхронно
    }

    Camera2D camera{.position = {0.0f, 0.0f}, .zoom = 1.0f};
    double last_time = WindowSystem::Window::time();
    double stats_timer = 0.0;

    for (int frame = 0; !window.should_close() && frame != max_frames; ++frame) {
        window.poll_events();
        const double now = WindowSystem::Window::time();
        const auto dt = static_cast<float>(now - last_time);
        last_time = now;

        // --- обновление
        for (Creature& c : creatures) {
            c.position += c.velocity * dt;
            if (std::abs(c.position.x) > 640.0f) c.velocity.x = -c.velocity.x;
            if (std::abs(c.position.y) > 400.0f) c.velocity.y = -c.velocity.y;
        }
        advance_animations(animation_states, animations, dt);
        camera.zoom = 1.5f + 0.5f * static_cast<float>(std::sin(now * 0.3));
        camera.position = {static_cast<float>(std::cos(now * 0.2)) * 100.0f, 0.0f};

        const auto [width, height] = window.framebuffer_size();
        camera.viewport = {static_cast<float>(width), static_cast<float>(height)};

        // --- отрисовка
        if (!device.begin_frame(width, height)) {
            device.end_frame();
            continue; // окно свёрнуто
        }
        renderer.clear(Color::from_rgba(0x1B1B1FFF));
        renderer.begin(camera);

        for (int y = -12; y < 12; ++y) {                // фон: слой -10, одна текстура
            for (int x = -20; x < 20; ++x) {
                renderer.draw(SpriteInstance{.position = {static_cast<float>(x) * 32.0f, static_cast<float>(y) * 32.0f}, .size = {32.0f, 32.0f},
                                             .pivot = {0.0f, 0.0f}, .texture = ground, .layer = -10});
            }
        }
        for (std::size_t i = 0; i < creatures.size(); ++i) {  // существа: слой 0
            renderer.draw(SpriteInstance{
                .position = creatures[i].position,
                .size = {32.0f, 32.0f},
                .uv = current_uv(animation_states[i], animations),
                .texture = creature_sheet,
                .flip = creatures[i].velocity.x < 0.0f ? SpriteFlip::X : SpriteFlip::None,
            });
        }
        renderer.draw_rect({{-640.0f, -400.0f}, {1280.0f, 800.0f}}, 4.0f, Colors::yellow, 10); // граница мира
        renderer.draw_line(creatures[0].position, creatures[1].position, 2.0f, Colors::red.with_alpha(160), 10);

        const RenderStats stats = renderer.end();
        device.end_frame();
        window.swap_buffers();

        stats_timer += dt;
        if (stats_timer >= 1.0) {
            stats_timer = 0.0;
            std::println("{:.0f} fps | quads {} | draw calls {} | texture binds {}", 1.0 / dt, stats.quads,
                         stats.draw_calls, stats.texture_binds);
        }
    }
    return 0;
}
