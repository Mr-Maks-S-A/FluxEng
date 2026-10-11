/**
 * @file test_core_app.cpp
 * @brief Core + все модули: настоящий цикл App с игрой-зондом.
 *
 * Зонд проверяет то, что видно только при работе модулей вместе:
 * - порядок хуков кадра (frame → tick → render_3d → render → render_overlay);
 * - ввод, внедрённый в окно между кадрами (Window::inject_*), доходит до шины как `platform.key` и
 *   `platform.mouse_button` — ни одно нажатие не теряется;
 * - память тика: выделенное в тике N читается в N+1 (как события шины);
 * - JobSystem доступен игре; шрифт интерфейса загружен; скриншот сохраняется в PNG и читается обратно.
 */

#include <Core/Core.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace es = EventSystem;
using namespace RendererSystem;

namespace {

class ProbeGame final : public Core::Game {
public:
    std::vector<std::string> calls;
    int frames = 0;
    int ticks = 0;
    int key_presses = 0;
    int clicks_pressed = 0;
    int clicks_released = 0;
    bool tick_memory_ok = true;
    double job_sum = 0.0;
    glm::vec2 text_size{0.0f};

    void setup(Core::App& app) override {
        record("setup");
        const es::ModuleId id = app.bus().declare_module("Probe").consumes<Core::KeyEvent>().consumes<Core::MouseButtonEvent>();
        keys = app.bus().reader<Core::KeyEvent>(id);
        clicks = app.bus().reader<Core::MouseButtonEvent>(id);
    }

    void frame(Core::App& app, float /*seconds*/) override {
        record("frame");
        if (++frames == 3) {
            // Между кадрами, как будто ОС прислала: клавиша и щелчок (нажать + отпустить в одном кадре).
            app.window().inject_key(InputSystem::Key::K, InputSystem::Transition::Press);
            app.window().inject_key(InputSystem::Key::K, InputSystem::Transition::Release);
            app.window().inject_mouse_button(InputSystem::MouseButton::Left, InputSystem::Transition::Press);
            app.window().inject_mouse_button(InputSystem::MouseButton::Left, InputSystem::Transition::Release);
        }
    }

    void tick(Core::App& app) override {
        record("tick");
        for (const Core::KeyEvent& k : keys.events()) {
            if (k.code() == InputSystem::Key::K && k.pressed()) ++key_presses;
        }
        for (const Core::MouseButtonEvent& c : clicks.events()) {
            if (c.which() != InputSystem::MouseButton::Left) continue;
            (c.pressed() ? clicks_pressed : clicks_released) += 1;
        }
        // Память тика: значение прошлого тика всё ещё на месте, новое — в текущей арене.
        if (previous != nullptr) {
            tick_memory_ok &= app.previous_tick_arena().owns(previous) && *previous == ticks - 1;
        }
        previous = app.tick_arena().push<int>();
        *previous = ticks;
        // JobSystem в руках игры.
        job_sum += JobSystem::parallel_reduce(app.jobs(), 1000, 64, 0.0, [](std::size_t b, std::size_t e) {
            double s = 0.0;
            for (std::size_t i = b; i < e; ++i) s += static_cast<double>(i);
            return s;
        }, [](double a, double b) { return a + b; });
        ++ticks;
    }

    void render_3d(Core::App& app, Renderer3D& r) override {
        record("render_3d");
        const glm::vec2 vp = app.camera().viewport;
        r.begin(Camera3D{.position = {0, 2, 4}, .viewport = vp});
        r.draw_shape(Renderer3D::Shape::Cube, glm::mat4{1.0f}, {.color = Colors::yellow});
        r.end();
    }

    void render(Core::App&, Renderer2D&) override { record("render"); }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        record("overlay");
        text_size = r.draw_text(app.ui_font(), "Проверка: Core + RendererSystem", {8, 8}, {.size = 20});
    }

    void shutdown(Core::App&) override { record("shutdown"); }

private:
    void record(const char* name) {
        if (calls.size() < 16 || std::string_view(name) == "shutdown") calls.emplace_back(name);
    }

    es::EventReader<Core::KeyEvent> keys;
    es::EventReader<Core::MouseButtonEvent> clicks;
    int* previous = nullptr; // указатель в арену тика
};

} // namespace

namespace {

void run_probe(Backend backend) {
    if (!backend_compiled(backend)) {
        MESSAGE(to_string(backend) << " backend is not compiled in — test skipped");
        return;
    }
    const std::filesystem::path screenshot =
        std::filesystem::temp_directory_path() / std::format("flux_integration_probe_{}.png", to_string(backend));
    std::filesystem::remove(screenshot);

    std::optional<Core::App> app;
    try {
        app.emplace(Core::AppConfig{.title = "IntegrationProbe", .width = 320, .height = 240, .visible = false,
                                    .max_ticks = 12, .screenshot = screenshot.string(), .threads = 2, .backend = backend});
    } catch (const std::exception& error) {
        MESSAGE("Core::App cannot start here (" << error.what() << ") — GPU test skipped");
        return;
    }
    CHECK(app->device().backend() == backend);
    ProbeGame game;
    CHECK(app->run(game) == 0);

    // Порядок хуков: setup, затем по кадрам frame → tick → render_3d → render → overlay.
    const std::vector<std::string> expected_prefix{"setup", "frame", "tick", "render_3d", "render", "overlay", "frame", "tick"};
    REQUIRE(game.calls.size() >= expected_prefix.size());
    CHECK(std::equal(expected_prefix.begin(), expected_prefix.end(), game.calls.begin()));
    CHECK(game.calls.back() == "shutdown");

    CHECK(game.ticks == 12);                 // lockstep: ровно max_ticks тиков
    CHECK(game.key_presses == 1);            // клавиша дошла до шины
    CHECK(game.clicks_pressed == 1);         // и нажатие, и отпускание в одном кадре — оба события
    CHECK(game.clicks_released == 1);
    CHECK(game.tick_memory_ok);
    CHECK(game.job_sum == doctest::Approx(12.0 * 999.0 * 1000.0 / 2.0));
    CHECK(app->ui_font().valid());
    CHECK(game.text_size.x > 100.0f);
    CHECK(app->renderer3d().last_stats().draws == 1);
    CHECK(app->device().validation_messages() == 0);

    const auto image = Image::load(screenshot);
    REQUIRE_MESSAGE(image.has_value(), image.error());
    CHECK(image->width() > 0);
    // Куб жёлтый: в кадре есть жёлтые пиксели — значит, 3D-проход дошёл до экрана (или до его замены без окна).
    const auto yellow = std::ranges::count_if(image->pixels(), [](Color c) { return c.r > 150 && c.g > 150 && c.b < 90; });
    CHECK(yellow > 100);
    std::filesystem::remove(screenshot);
}

} // namespace

TEST_SUITE("Core + all modules") {
    TEST_CASE("gpu: Core::App on OpenGL drives a game through every hook, delivers injected input via the bus and saves a screenshot") {
        run_probe(Backend::OpenGL);
    }

    TEST_CASE("gpu: Core::App on Vulkan does the same (hidden window, offscreen screen target, validation clean)") {
        run_probe(Backend::Vulkan);
    }
}
