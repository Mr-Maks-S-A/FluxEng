#include <WindowSystem/Window.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <utility>

using WindowSystem::Window;
using WindowSystem::WindowConfig;

namespace {

/// Скрытое окно для теста; nullopt, если дисплея нет (тест тогда пропускается).
std::optional<Window> hidden_window(WindowConfig config = {}) {
    config.visible = false;
    config.width = 320;
    config.height = 240;
    auto created = Window::create(config);
    if (!created) {
        MESSAGE("окно не создано, тест пропущен: " << created.error());
        return std::nullopt;
    }
    return std::move(*created);
}

} // namespace

TEST_SUITE("WindowSystem.Window") {

TEST_CASE("пустое окно (ZII): все запросы безопасны") {
    Window window;
    CHECK_FALSE(window);
    CHECK(window.should_close());
    CHECK(window.framebuffer_size() == WindowSystem::Size{});
    CHECK(window.title().empty());
    window.poll_events();
    window.swap_buffers();
    window.request_close();
    window.set_title("ignored");
    CHECK_FALSE(window.input().down(0));
}

TEST_CASE("создание: размеры, заголовок, vsync, закрытие") {
    auto window = hidden_window({.title = "test"});
    if (!window) return;
    const int open_before = Window::open_windows();
    CHECK(open_before >= 1);

    CHECK(window->native_handle() != nullptr);
    CHECK(window->title() == "test");
    CHECK(window->window_size().width == 320);
    CHECK(window->framebuffer_size().width > 0);
    CHECK(window->content_scale() > 0.0f);

    window->set_title("renamed");
    CHECK(window->title() == "renamed");

    window->set_vsync(false);
    CHECK_FALSE(window->vsync());

    CHECK_FALSE(window->should_close());
    window->request_close();
    CHECK(window->should_close());
}

TEST_CASE("счётчик окон: закрытие последнего завершает GLFW, повторное создание работает") {
    {
        auto a = hidden_window();
        if (!a) return;
        auto b = hidden_window();
        REQUIRE(b);
        CHECK(Window::open_windows() == 2);
    }
    CHECK(Window::open_windows() == 0);
    auto again = hidden_window();
    REQUIRE(again);
    CHECK(Window::open_windows() == 1);
}

TEST_CASE("внедрённые события идут тем же путём: input + подписчики") {
    auto window = hidden_window();
    if (!window) return;

    int keys = 0;
    int last_key = 0;
    window->events().key.subscribe([&](int key, int) {
        ++keys;
        last_key = key;
    });
    window->events().key.subscribe([&](int, int) { ++keys; }); // второй слушатель

    window->poll_events();
    window->inject_key(GLFW_KEY_SPACE, GLFW_PRESS);
    window->inject_scroll(0.0, 2.0);
    window->inject_cursor(10.0, 20.0);

    CHECK(keys == 2);
    CHECK(last_key == GLFW_KEY_SPACE);
    CHECK(window->input().pressed(GLFW_KEY_SPACE));
    CHECK(window->input().scroll().y == 2.0);
    CHECK(window->input().cursor().x == 10.0);

    window->poll_events(); // новый кадр
    CHECK_FALSE(window->input().pressed(GLFW_KEY_SPACE));
    CHECK(window->input().down(GLFW_KEY_SPACE));
}

TEST_CASE("Esc закрывает окно только с close_on_escape") {
    auto plain = hidden_window();
    if (!plain) return;
    plain->inject_key(GLFW_KEY_ESCAPE, GLFW_PRESS);
    CHECK_FALSE(plain->should_close());

    auto closing = hidden_window({.close_on_escape = true});
    REQUIRE(closing);
    closing->inject_key(GLFW_KEY_ESCAPE, GLFW_PRESS);
    CHECK(closing->should_close());
}

TEST_CASE("перемещение: обработчики продолжают работать") {
    auto window = hidden_window();
    if (!window) return;
    int calls = 0;
    window->events().key.subscribe([&](int, int) { ++calls; });

    Window moved = std::move(*window);
    CHECK_FALSE(*window);
    moved.inject_key(GLFW_KEY_A, GLFW_PRESS);
    CHECK(calls == 1);
    CHECK(moved.input().down(GLFW_KEY_A));
}

}
