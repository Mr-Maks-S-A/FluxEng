// Окно без экрана: всё, что делает Window поверх платформы (ввод, события, закрытие), проверяется без дисплея и GPU.

#include <WindowSystem/Window.hpp>

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using namespace InputSystem;
using WindowSystem::ClientApi;
using WindowSystem::Window;
using WindowSystem::WindowBackend;
using WindowSystem::WindowConfig;

namespace {
Window headless(WindowConfig config = {}) {
    config.backend = WindowBackend::Headless;
    config.width = 640;
    config.height = 360;
    auto created = Window::create(config);
    REQUIRE_MESSAGE(created.has_value(), created.error());
    return std::move(*created);
}
} // namespace

TEST_SUITE("WindowSystem.Headless") {

TEST_CASE("создаётся без дисплея: размеры из конфигурации, графики нет") {
    Window window = headless({.title = "ci"});
    CHECK(window);
    CHECK(window.backend() == WindowBackend::Headless);
    CHECK(window.api() == ClientApi::None); // даже если просили OpenGL по умолчанию
    CHECK(window.window_size() == WindowSystem::Size{640, 360});
    CHECK(window.framebuffer_size() == WindowSystem::Size{640, 360});
    CHECK(window.content_scale() == 1.0f);
    CHECK(window.title() == "ci");
    CHECK(window.native_handle() == nullptr);
    CHECK_FALSE(window.should_close());
    window.set_title("renamed");
    CHECK(window.title() == "renamed");
    window.swap_buffers();
    window.poll_events();
}

TEST_CASE("headless не трогает GLFW: счётчик окон GLFW не растёт") {
    const int before = Window::open_windows();
    Window window = headless();
    CHECK(Window::open_windows() == before);
}

TEST_CASE("поверхности Vulkan нет") {
    Window window = headless();
    const auto surface = window.create_vulkan_surface(0);
    REQUIRE_FALSE(surface.has_value());
    CHECK(surface.error().find("headless") != std::string::npos);
}

TEST_CASE("внедрённые события: сначала input(), потом подписчики typed, потом общая подписка") {
    Window window = headless();
    std::vector<std::string> order;
    window.events().key.subscribe([&](Key key, Transition t) {
        order.push_back("typed");
        CHECK(window.input().pressed(key)); // к моменту вызова состояние уже обновлено
        CHECK(t == Transition::Press);
    });
    window.events().input.subscribe([&](const InputEvent& e) { order.push_back(std::holds_alternative<KeyInput>(e) ? "any" : "other"); });

    window.poll_events();
    window.inject_key(Key::Space, Transition::Press);
    CHECK(order == std::vector<std::string>{"typed", "any"});
    CHECK(window.input().down(Key::Space));

    window.poll_events(); // новый кадр
    CHECK_FALSE(window.input().pressed(Key::Space));
    CHECK(window.input().down(Key::Space));
}

TEST_CASE("все виды ввода доходят до состояния и подписчиков") {
    Window window = headless();
    int buttons = 0, cursor = 0, scroll = 0, chars = 0, focus = 0, pads = 0;
    window.events().mouse_button.subscribe([&](MouseButton b, Transition) { buttons += b == MouseButton::Right; });
    window.events().cursor.subscribe([&](double, double) { ++cursor; });
    window.events().scroll.subscribe([&](double, double) { ++scroll; });
    window.events().character.subscribe([&](std::uint32_t) { ++chars; });
    window.events().focus.subscribe([&](bool) { ++focus; });
    window.events().input.subscribe([&](const InputEvent& e) { pads += std::holds_alternative<GamepadButtonInput>(e); });

    window.poll_events();
    window.inject_mouse_button(MouseButton::Right, Transition::Press);
    window.inject_cursor(11, 22);
    window.inject_scroll(0, 3);
    window.inject_char(0x43F);
    window.inject(FocusInput{false});
    window.inject(GamepadConnectionInput{0, true});
    window.inject(GamepadButtonInput{0, GamepadButton::A, Transition::Press});

    CHECK((buttons == 1 && cursor == 1 && scroll == 1 && chars == 1 && focus == 1 && pads == 1));
    CHECK(window.input().cursor().x == 11);
    CHECK(window.input().scroll().y == 3);
    CHECK(window.input().text().size() == 1);
    CHECK(window.input().gamepad_connected(0));
    CHECK(window.input().gamepad_pressed(0, GamepadButton::A));
    CHECK_FALSE(window.input().focused());
}

TEST_CASE("потеря фокуса отпускает зажатое") {
    Window window = headless();
    window.poll_events();
    window.inject_key(Key::W, Transition::Press);
    window.poll_events();
    CHECK(window.input().down(Key::W));
    window.inject(FocusInput{false});
    CHECK_FALSE(window.input().down(Key::W));
    CHECK(window.input().released(Key::W));
}

TEST_CASE("Esc закрывает окно только с close_on_escape") {
    Window plain = headless();
    plain.inject_key(Key::Escape, Transition::Press);
    CHECK_FALSE(plain.should_close());
    Window closing = headless({.close_on_escape = true});
    closing.inject_key(Key::Escape, Transition::Press);
    CHECK(closing.should_close());
    Window manual = headless();
    manual.request_close();
    CHECK(manual.should_close());
}

TEST_CASE("перемещение: обработчики продолжают работать, исходное окно пусто") {
    Window window = headless();
    int calls = 0;
    window.events().key.subscribe([&](Key, Transition) { ++calls; });
    Window moved = std::move(window);
    CHECK_FALSE(window);
    CHECK(window.should_close());
    moved.inject_key(Key::A, Transition::Press);
    CHECK(calls == 1);
    CHECK(moved.input().down(Key::A));

    Window assigned;
    assigned = std::move(moved);
    assigned.inject_key(Key::B, Transition::Press);
    CHECK(assigned.input().down(Key::B));
}

TEST_CASE("курсор в framebuffer: без HiDPI совпадает с курсором окна") {
    Window window = headless();
    window.inject_cursor(100, 50);
    CHECK(window.cursor_in_framebuffer().x == 100);
    CHECK(window.cursor_in_framebuffer().y == 50);
}

TEST_CASE("режим курсора запоминается; time() монотонно растёт") {
    Window window = headless();
    window.set_cursor_mode(WindowSystem::CursorMode::Captured);
    CHECK(window.cursor_mode() == WindowSystem::CursorMode::Captured);
    const double a = Window::time();
    const double b = Window::time();
    CHECK(b >= a);
    window.set_vsync(false);
    CHECK_FALSE(window.vsync());
}

TEST_CASE("пустое окно (ZII): все запросы безопасны, подписки допустимы, внедрение ничего не делает") {
    Window window;
    CHECK_FALSE(window);
    CHECK(window.should_close());
    CHECK(window.framebuffer_size() == WindowSystem::Size{});
    CHECK(window.title().empty());
    CHECK(window.native_handle() == nullptr);
    window.poll_events();
    window.swap_buffers();
    window.request_close();
    window.set_title("ignored");
    window.inject_key(Key::A, Transition::Press);
    CHECK_FALSE(window.input().down(Key::A));
    window.events().key.subscribe([](Key, Transition) {});
    CHECK_FALSE(window.create_vulkan_surface(0).has_value());
}

} // TEST_SUITE
