#include <Core/App.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string_view>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace Core {

using namespace RendererSystem;
namespace es = EventSystem;

App* App::s_active = nullptr;

AppConfig parse_args(AppConfig config, int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--frames") {
            config.max_frames = std::atoi(argv[i + 1]);
        } else if (std::string_view(argv[i]) == "--ticks") {
            config.max_ticks = std::atoi(argv[i + 1]);
        } else if (std::string_view(argv[i]) == "--screenshot") {
            config.screenshot = argv[i + 1];
        }
    }
    return config;
}

App::App(AppConfig config)
    : m_config(std::move(config)), m_window(m_config.width, m_config.height, m_config.title, true) {
    m_step.ticks_per_second = m_config.ticks_per_second;
    m_step.lockstep = m_config.max_ticks >= 0;
    if (m_window.getNativeWindow() == nullptr) {
        throw std::runtime_error("cannot create window / OpenGL context");
    }
    auto renderer = Renderer2D::create();
    if (!renderer) {
        throw std::runtime_error(renderer.error());
    }
    m_renderer.emplace(std::move(*renderer));

    // Модуль платформы: мост «колбэки окна → события шины».
    m_platform = m_bus.declare_module("Platform")
                     .produces<KeyEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024})
                     .produces<MouseButtonEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024});
    m_key_out = m_bus.writer<KeyEvent>(m_platform);
    m_mouse_out = m_bus.writer<MouseButtonEvent>(m_platform);

    m_window.onKeyPress = [this](int key, int action) { on_key(key, action); };

    s_active = this;
    glfwSetScrollCallback(m_window.getNativeWindow(), [](GLFWwindow*, double, double y) {
        if (s_active != nullptr) {
            s_active->m_pending_scroll += static_cast<float>(y);
        }
    });
}

void App::on_key(int key, int action) {
    m_key_out.emit(KeyEvent{.key = key, .action = action});
    if (action != GLFW_PRESS) {
        return;
    }
    switch (key) {
        case GLFW_KEY_SPACE: m_step.paused = !m_step.paused; break;
        case GLFW_KEY_EQUAL:
        case GLFW_KEY_KP_ADD: m_step.speed = std::min(m_step.speed * 2, 8); break;
        case GLFW_KEY_MINUS:
        case GLFW_KEY_KP_SUBTRACT: m_step.speed = std::max(m_step.speed / 2, 1); break;
        case GLFW_KEY_F1: print_event_report(); break;
        default: break;
    }
}

void App::poll_input(float frame_seconds) {
    GLFWwindow* native = m_window.getNativeWindow();

    // Панорама камерой — в домене кадра, работает и на паузе.
    glm::vec2 pan{0.0f};
    if (glfwGetKey(native, GLFW_KEY_A) == GLFW_PRESS || glfwGetKey(native, GLFW_KEY_LEFT) == GLFW_PRESS) pan.x -= 1;
    if (glfwGetKey(native, GLFW_KEY_D) == GLFW_PRESS || glfwGetKey(native, GLFW_KEY_RIGHT) == GLFW_PRESS) pan.x += 1;
    if (glfwGetKey(native, GLFW_KEY_W) == GLFW_PRESS || glfwGetKey(native, GLFW_KEY_UP) == GLFW_PRESS) pan.y -= 1;
    if (glfwGetKey(native, GLFW_KEY_S) == GLFW_PRESS || glfwGetKey(native, GLFW_KEY_DOWN) == GLFW_PRESS) pan.y += 1;
    m_camera.position += pan * (500.0f * frame_seconds / m_camera.zoom);

    double mx = 0.0;
    double my = 0.0;
    glfwGetCursorPos(native, &mx, &my);
    int window_w = 0;
    int window_h = 0;
    glfwGetWindowSize(native, &window_w, &window_h);
    // Курсор приходит в координатах окна, камера — в пикселях framebuffer'а (HiDPI).
    const glm::vec2 scale = window_w > 0 ? m_camera.viewport / glm::vec2(window_w, window_h) : glm::vec2(1.0f);
    m_input.mouse_screen = glm::vec2(mx, my) * scale;

    // Зум к курсору: точка мира под курсором остаётся на месте.
    if (m_pending_scroll != 0.0f) {
        const glm::vec2 before = m_camera.screen_to_world(m_input.mouse_screen);
        m_camera.zoom = std::clamp(m_camera.zoom * std::pow(1.15f, m_pending_scroll), 0.1f, 20.0f);
        m_camera.position += before - m_camera.screen_to_world(m_input.mouse_screen);
        m_pending_scroll = 0.0f;
    }
    m_input.mouse_world = m_camera.screen_to_world(m_input.mouse_screen);

    constexpr int buttons[3] = {GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT, GLFW_MOUSE_BUTTON_MIDDLE};
    for (std::size_t i = 0; i < 3; ++i) {
        const bool down = glfwGetMouseButton(native, buttons[i]) == GLFW_PRESS;
        m_input.pressed[i] = down && !m_input.down[i];
        if (down != m_input.down[i]) {
            m_mouse_out.emit(MouseButtonEvent{.button = buttons[i], .action = down ? GLFW_PRESS : GLFW_RELEASE,
                                              .world_x = m_input.mouse_world.x, .world_y = m_input.mouse_world.y});
        }
        m_input.down[i] = down;
    }
}

int App::run(Game& game) {
    game.setup(*this);

    // Контракты модулей известны после setup — показываем граф сразу.
    const es::EventGraph graph = m_bus.build_graph();
    std::println("===== {} : event graph =====\n{}", m_config.title, graph.to_text());
    std::string dot_name = m_config.title;
    std::ranges::replace(dot_name, ' ', '_');
    dot_name += "_events.dot";
    std::ofstream(dot_name) << graph.to_dot();
    std::println("graph written to {} (dot -Tsvg {} -o graph.svg)\n", dot_name, dot_name);

    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(m_window.getNativeWindow(), &fb_w, &fb_h);
    m_camera.viewport = {static_cast<float>(fb_w), static_cast<float>(fb_h)};
    const glm::vec2 world = game.world_size();
    m_camera.position = world * 0.5f;
    m_camera.zoom = std::min(m_camera.viewport.x / world.x, m_camera.viewport.y / world.y) * 0.95f;

    double last = glfwGetTime();
    double title_timer = 0.0;
    int title_frames = 0;

    for (int frame = 0; !m_window.shouldClose(); ++frame) {
        const double now = glfwGetTime();
        const double frame_dt = std::min(now - last, 0.25);
        last = now;

        glfwGetFramebufferSize(m_window.getNativeWindow(), &fb_w, &fb_h);
        m_camera.viewport = {static_cast<float>(std::max(fb_w, 1)), static_cast<float>(std::max(fb_h, 1))};
        poll_input(static_cast<float>(frame_dt));

        // --- симуляция
        for (int steps = m_step.advance(frame_dt); steps > 0; --steps) {
            game.tick(*this);
            m_bus.advance_tick();
        }

        const bool last_frame = (m_config.max_frames >= 0 && frame + 1 >= m_config.max_frames) ||
                                (m_config.max_ticks >= 0 && m_bus.current_tick() >= static_cast<es::Tick>(m_config.max_ticks));

        // --- отрисовка
        Renderer2D& r = *m_renderer;
        r.set_viewport(fb_w, fb_h);
        r.clear(Color::from_rgba(0x15151AFF));
        r.begin(m_camera);
        game.render(*this, r);
        r.end();

        r.begin(Camera2D{.position = m_camera.viewport * 0.5f, .viewport = m_camera.viewport});
        game.render_overlay(*this, r);
        draw_bus_overlay();
        r.end();

        if (!m_config.screenshot.empty() && last_frame) {
            save_screenshot(fb_w, fb_h);
        }
        m_window.update();

        title_timer += frame_dt;
        ++title_frames;
        if (title_timer >= 0.5) {
            update_title(game, title_frames / title_timer);
            title_timer = 0.0;
            title_frames = 0;
        }
        if (last_frame) {
            break;
        }
    }

    std::println("\n===== {} : finished at tick {} =====", m_config.title, m_bus.current_tick());
    print_event_report();
    s_active = nullptr;
    return 0;
}

void App::draw_bus_overlay() {
    // Текста в рендере пока нет, поэтому шина показана столбиками:
    // высота — сколько событий канала видно в этом тике (лог-шкала), красная шапка — были отброшенные.
    Renderer2D& r = *m_renderer;
    const float base = m_camera.viewport.y - 12.0f;
    constexpr Color palette[] = {Color::from_rgba(0x4FC3F7FF), Color::from_rgba(0x81C784FF),
                                 Color::from_rgba(0xFFB74DFF), Color::from_rgba(0xBA68C8FF),
                                 Color::from_rgba(0xE57373FF), Color::from_rgba(0xFFF176FF)};

    const float width = static_cast<float>(m_bus.channel_count()) * 14.0f + 8.0f;
    r.fill_rect({{4.0f, base - 90.0f}, {width, 96.0f}}, Color{0, 0, 0, 150}, 0);
    for (std::size_t i = 0; i < m_bus.channel_count(); ++i) {
        const es::ChannelStats stats = m_bus.channel_at(i).stats();
        const float height = 3.0f + 10.0f * std::log2(1.0f + static_cast<float>(stats.readable));
        const float x = 10.0f + static_cast<float>(i) * 14.0f;
        r.fill_rect({{x, base - height}, {10.0f, height}}, palette[i % std::size(palette)], 1);
        if (stats.total_dropped > 0) {
            r.fill_rect({{x, base - 88.0f}, {10.0f, 4.0f}}, Colors::red, 2);
        }
    }

    // Пауза и скорость — справа сверху.
    const float right = m_camera.viewport.x - 12.0f;
    if (m_step.paused) {
        r.fill_rect({{right - 26.0f, 12.0f}, {9.0f, 28.0f}}, Colors::white, 3);
        r.fill_rect({{right - 11.0f, 12.0f}, {9.0f, 28.0f}}, Colors::white, 3);
    } else {
        for (int i = 0; i < m_step.speed; ++i) {
            r.fill_rect({{right - 10.0f - static_cast<float>(i) * 12.0f, 12.0f}, {8.0f, 16.0f}}, Colors::yellow, 3);
        }
    }
}

void App::save_screenshot(int width, int height) const {
    // В RendererSystem нет чтения окна (только Framebuffer::read_pixels), поэтому glReadPixels напрямую.
    Image image(width, height);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels().data());
    image.flip_vertically();
    if (stbi_write_png(m_config.screenshot.c_str(), width, height, 4, image.pixels().data(), width * 4) != 0) {
        std::println("screenshot saved to {}", m_config.screenshot);
    }
}

void App::update_title(Game& game, double fps) {
    // В WindowSystem нет set_title() — обращаемся к GLFW напрямую.
    const std::string title =
        std::format("{} | tick {} | {} | {:.0f} fps | {} draw calls | {}", m_config.title, m_bus.current_tick(),
                    m_step.paused ? "PAUSED" : std::format("x{}", m_step.speed), fps, m_renderer->last_stats().draw_calls,
                    game.status());
    glfwSetWindowTitle(m_window.getNativeWindow(), title.c_str());
}

void App::print_event_report() const {
    const es::EventGraph graph = m_bus.build_graph();
    std::println("{}", graph.to_text());

    const es::ModuleOrder order = graph.module_order();
    std::print("module order:");
    for (const es::ModuleId id : order.order) {
        std::print(" {}", m_bus.modules().info(id).name);
    }
    for (const es::ModuleId id : order.cyclic) {
        std::print(" [cycle: {}]", m_bus.modules().info(id).name);
    }
    std::println("");

    for (const es::EventId id : graph.unconsumed_events()) {
        std::println("warning: nobody consumes '{}'", graph.find_event(id)->name);
    }
    for (const es::EventId id : graph.unproduced_events()) {
        std::println("warning: nobody produces '{}'", graph.find_event(id)->name);
    }

    std::println("{:<28} {:>12} {:>10} {:>10} {:>12}", "channel", "emitted", "peak/tick", "dropped", "memory");
    for (std::size_t i = 0; i < m_bus.channel_count(); ++i) {
        const es::IChannel& channel = m_bus.channel_at(i);
        const es::ChannelStats s = channel.stats();
        std::println("{:<28} {:>12} {:>10} {:>10} {:>10} KB", channel.name(), s.total_emitted, s.peak_per_tick,
                     s.total_dropped, s.allocated_bytes / 1024);
    }
}

} // namespace Core
