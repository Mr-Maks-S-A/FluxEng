/**
 * @example 01_window_input.cpp
 * Окно, кадр, ввод опросом и подписками. `--frames N` — выйти через N кадров (smoke-тест).
 */

#include <WindowSystem/Window.hpp>

#include <cstdlib>
#include <format>
#include <print>
#include <string_view>

int main(int argc, char** argv) {
    int max_frames = -1;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--frames") max_frames = std::atoi(argv[i + 1]);
    }

    auto created = WindowSystem::Window::create({.title = "WindowSystem — input", .width = 800, .height = 450});
    if (!created) {
        std::println(stderr, "cannot open a window: {}", created.error());
        return 1;
    }
    WindowSystem::Window& window = *created;

    // Подписки: сколько угодно обработчиков на одно событие.
    window.events().key.subscribe([&](int key, int action) {
        if (key == GLFW_KEY_ESCAPE && action == WindowSystem::action_press) window.request_close();
    });
    window.events().framebuffer_resized.subscribe(
        [](int width, int height) { std::println("framebuffer resized: {}x{}", width, height); });

    const double start = WindowSystem::Window::time();
    for (int frame = 0; !window.should_close() && frame != max_frames; ++frame) {
        window.poll_events();
        const WindowSystem::InputState& input = window.input();

        // Опрос: состояние за кадр.
        if (input.pressed(GLFW_KEY_SPACE)) std::println("space pressed at {:.2f} s", WindowSystem::Window::time() - start);
        if (input.mouse_pressed(GLFW_MOUSE_BUTTON_LEFT)) {
            const WindowSystem::Vec2d fb = window.cursor_in_framebuffer();
            std::println("click at framebuffer ({:.0f}, {:.0f})", fb.x, fb.y);
        }
        if (input.scroll().y != 0.0) std::println("scroll {:+.1f}", input.scroll().y);

        // Цвет фона зависит от курсора — видно, что ввод живой.
        const WindowSystem::Size fb = window.framebuffer_size();
        const WindowSystem::Size ws = window.window_size();
        const float red = ws.width > 0 ? static_cast<float>(input.cursor().x / ws.width) : 0.0f;
        glViewport(0, 0, fb.width, fb.height);
        glClearColor(red, 0.15f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (frame % 30 == 0) {
            window.set_title(std::format("WindowSystem — input | frame {} | vsync {}", frame, window.vsync()));
        }
        window.swap_buffers();
    }
    std::println("closed after {:.2f} s", WindowSystem::Window::time() - start);
}
