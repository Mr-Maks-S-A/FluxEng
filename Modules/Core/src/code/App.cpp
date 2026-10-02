#include <Core/App.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string_view>

namespace Core {

using namespace RendererSystem;
namespace es = EventSystem;

AppConfig parse_args(AppConfig config, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--frames" && has_value) {
            config.max_frames = std::atoi(argv[++i]);
        } else if (arg == "--ticks" && has_value) {
            config.max_ticks = std::atoi(argv[++i]);
        } else if (arg == "--threads" && has_value) {
            config.threads = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--screenshot" && has_value) {
            config.screenshot = argv[++i];
        } else if (arg == "--backend" && has_value) {
            const auto backend = parse_backend(argv[++i]);
            if (!backend) throw std::invalid_argument(std::format("unknown backend '{}' (use gl or vulkan)", argv[i]));
            config.backend = *backend;
        } else if (arg == "--validation") {
            config.validation = true;
        } else {
            config.extra_args.emplace_back(arg); // игре: например, --agents 100000
        }
    }
    return config;
}

namespace {

WindowSystem::Window open_window(const AppConfig& config) {
    const bool vulkan = config.backend == Backend::Vulkan;
    auto window = WindowSystem::Window::create(
        {.title = std::format("{} [{}]", config.title, to_string(config.backend)), .width = config.width, .height = config.height,
         .visible = config.visible,
         .vsync = config.visible, // скрытому окну незачем ждать монитор
         .api = vulkan ? WindowSystem::ClientApi::None : WindowSystem::ClientApi::OpenGL});
    if (!window) {
        throw std::runtime_error(window.error());
    }
    return std::move(*window);
}

std::unique_ptr<RHI::Device> open_device(const AppConfig& config, const WindowSystem::Window& window) {
    RHI::DeviceConfig device_config{.backend = config.backend, .validation = config.validation, .vsync = config.visible};
    if (config.backend == Backend::Vulkan && config.visible) { // скрытому окну swapchain не нужен: App рисует в свою цель
        device_config.instance_extensions = WindowSystem::Window::vulkan_instance_extensions();
        device_config.create_surface = [&window](std::uintptr_t instance) { return window.create_vulkan_surface(instance); };
    }
    auto device = RHI::Device::create(device_config);
    if (!device) {
        throw std::runtime_error(std::format("{} device: {}", to_string(config.backend), device.error()));
    }
    return std::move(*device);
}

} // namespace

App::App(AppConfig config)
    : m_config(std::move(config)), m_window(open_window(m_config)),
      m_jobs(JobSystem::SchedulerConfig{
          .threads = m_config.threads >= 0 ? static_cast<unsigned>(m_config.threads) : JobSystem::default_threads()}) {
    m_step.ticks_per_second = m_config.ticks_per_second;
    m_step.lockstep = m_config.max_ticks >= 0;
    m_device = open_device(m_config, m_window);
    std::println("render: {} — {} ({})", to_string(m_device->backend()), m_device->info().device_name, m_device->info().api_version);
    auto renderer = Renderer2D::create(*m_device);
    if (!renderer) {
        throw std::runtime_error(renderer.error());
    }
    m_renderer.emplace(std::move(*renderer));
    auto renderer3d = Renderer3D::create(*m_device);
    if (!renderer3d) {
        throw std::runtime_error(renderer3d.error());
    }
    m_renderer3d.emplace(std::move(*renderer3d));
    load_ui_fonts();

    // Модуль платформы: мост «колбэки окна → события шины».
    m_platform = m_bus.declare_module("Platform")
                     .produces<KeyEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024})
                     .produces<MouseButtonEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024});
    m_key_out = m_bus.writer<KeyEvent>(m_platform);
    m_mouse_out = m_bus.writer<MouseButtonEvent>(m_platform);

    m_window.events().key.subscribe([this](int key, int action) { on_key(key, action); });
    // Кнопки мыши — подпиской, как и клавиши: каждое нажатие и отпускание попадает в шину по порядку,
    // включая внедрённые (Window::inject_mouse_button) между кадрами — флаги кадра InputState их бы потеряли.
    m_window.events().mouse_button.subscribe([this](int button, int action) {
        const WindowSystem::Vec2d cursor = m_window.cursor_in_framebuffer();
        const glm::vec2 world = m_camera.screen_to_world({static_cast<float>(cursor.x), static_cast<float>(cursor.y)});
        m_mouse_out.emit(MouseButtonEvent{.button = button, .action = action, .world_x = world.x, .world_y = world.y});
    });
}

void App::load_ui_fonts() {
    const FontConfig config{.pixel_height = m_config.ui_font_size};
    auto regular = Font::load_system(config);
    if (regular) {
        std::println("ui font: {} ({} glyphs, atlas {}x{})", regular->source(), regular->glyph_count(),
                     regular->atlas().width(), regular->atlas().height());
        m_ui_font = m_renderer->add_font(std::move(*regular));
    } else {
        std::println("ui font: {} — using builtin ASCII font", regular.error());
        m_ui_font = m_renderer->add_font(Font::builtin(std::max(1, static_cast<int>(m_config.ui_font_size / 12.0f))));
    }
    auto bold = Font::load_system(config, true);
    m_ui_font_bold = bold ? m_renderer->add_font(std::move(*bold)) : m_ui_font;
}

void App::on_key(int key, int action) {
    m_key_out.emit(KeyEvent{.key = key, .action = action});
    if (action != GLFW_PRESS) {
        return;
    }
    if (key == m_config.pause_key) {
        m_step.paused = !m_step.paused;
        return;
    }
    switch (key) {
        case GLFW_KEY_EQUAL:
        case GLFW_KEY_KP_ADD: m_step.speed = std::min(m_step.speed * 2, 8); break;
        case GLFW_KEY_MINUS:
        case GLFW_KEY_KP_SUBTRACT: m_step.speed = std::max(m_step.speed / 2, 1); break;
        case GLFW_KEY_F1: print_event_report(); break;
        case GLFW_KEY_ESCAPE: m_window.request_close(); break;
        default: break;
    }
}

void App::poll_input(float frame_seconds) {
    const WindowSystem::InputState& input = m_window.input();

    // Панорама камерой — в домене кадра, работает и на паузе.
    if (m_config.camera_controls) {
        glm::vec2 pan{0.0f};
        if (input.down(GLFW_KEY_A) || input.down(GLFW_KEY_LEFT)) pan.x -= 1;
        if (input.down(GLFW_KEY_D) || input.down(GLFW_KEY_RIGHT)) pan.x += 1;
        if (input.down(GLFW_KEY_W) || input.down(GLFW_KEY_UP)) pan.y -= 1;
        if (input.down(GLFW_KEY_S) || input.down(GLFW_KEY_DOWN)) pan.y += 1;
        m_camera.position += pan * (500.0f * frame_seconds / m_camera.zoom);
    }

    // Курсор приходит в координатах окна, камера — в пикселях framebuffer'а (HiDPI): пересчёт в WindowSystem.
    const WindowSystem::Vec2d cursor = m_window.cursor_in_framebuffer();
    m_input.mouse_screen = {static_cast<float>(cursor.x), static_cast<float>(cursor.y)};

    // Зум к курсору: точка мира под курсором остаётся на месте.
    if (const auto scroll = static_cast<float>(input.scroll().y); scroll != 0.0f && m_config.camera_controls) {
        const glm::vec2 before = m_camera.screen_to_world(m_input.mouse_screen);
        m_camera.zoom = std::clamp(m_camera.zoom * std::pow(1.15f, scroll), 0.1f, 20.0f);
        m_camera.position += before - m_camera.screen_to_world(m_input.mouse_screen);
    }
    m_input.mouse_world = m_camera.screen_to_world(m_input.mouse_screen);

    constexpr int buttons[3] = {GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT, GLFW_MOUSE_BUTTON_MIDDLE};
    for (std::size_t i = 0; i < 3; ++i) {
        m_input.down[i] = input.mouse_down(buttons[i]);
        m_input.pressed[i] = input.mouse_pressed(buttons[i]);
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
    std::println("graph written to {} (dot -Tsvg {} -o graph.svg)", dot_name, dot_name);
    std::println("job threads: {} background + main (--threads N)\n", m_jobs.threads());

    WindowSystem::Size fb = m_window.framebuffer_size();
    int fb_w = fb.width;
    int fb_h = fb.height;
    m_camera.viewport = {static_cast<float>(fb_w), static_cast<float>(fb_h)};
    const glm::vec2 world = game.world_size();
    m_camera.position = world * 0.5f;
    m_camera.zoom = std::min(m_camera.viewport.x / world.x, m_camera.viewport.y / world.y) * 0.95f;

    double last = WindowSystem::Window::time();
    double title_timer = 0.0;
    int title_frames = 0;

    for (int frame = 0; !m_window.should_close(); ++frame) {
        m_window.poll_events(); // ввод этого кадра: InputState + подписчики (клавиши → шина)
        m_frame_memory.reset();
        const double now = WindowSystem::Window::time();
        const double frame_dt = std::min(now - last, 0.25);
        last = now;

        fb = m_window.framebuffer_size();
        fb_w = fb.width;
        fb_h = fb.height;
        m_camera.viewport = {static_cast<float>(std::max(fb_w, 1)), static_cast<float>(std::max(fb_h, 1))};
        poll_input(static_cast<float>(frame_dt));
        const bool drawable = m_device->begin_frame(fb_w, fb_h); // до Game::frame: игра может рисовать в текстуры
        game.frame(*this, static_cast<float>(frame_dt));
        m_bus.advance_frame(); // каналы домена Frame (интерфейс, ввод): живут и на паузе, когда тиков нет

        // --- симуляция
        for (int steps = m_step.advance(frame_dt); steps > 0; --steps) {
            game.tick(*this);
            m_bus.advance_tick();
            m_tick_memory.swap(); // память тика N доступна в N+1 как previous, затем очищается
        }

        const bool last_frame = (m_config.max_frames >= 0 && frame + 1 >= m_config.max_frames) ||
                                (m_config.max_ticks >= 0 && m_bus.current_tick() >= static_cast<es::Tick>(m_config.max_ticks));

        // --- отрисовка
        if (drawable) {
            Renderer2D& r = *m_renderer;
            bind_screen(fb_w, fb_h); // экран; Game::frame мог рисовать в текстуры
            m_renderer3d->clear(Color::from_rgba(m_config.clear_rgba));
            game.render_3d(*this, *m_renderer3d);
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
        }
        m_device->end_frame();
        m_window.swap_buffers(); // OpenGL; у окна для Vulkan ничего не делает

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

    m_device->wait_idle(); // ресурсы игры можно освобождать: GPU их больше не читает
    game.shutdown(*this);
    std::println("\n===== {} : finished at tick {} =====", m_config.title, m_bus.current_tick());
    print_event_report();
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

void App::bind_screen(int width, int height) {
    if (m_device->info().presents) {
        m_device->bind_target({});
        return;
    }
    // Устройство без окна: «экран» — своя цель размером с framebuffer окна.
    if (!m_offscreen || m_offscreen->width() != width || m_offscreen->height() != height) {
        auto target = RenderTarget::create(*m_device, std::max(width, 1), std::max(height, 1), {.depth = true});
        if (!target) throw std::runtime_error(target.error());
        m_offscreen.emplace(std::move(*target));
    }
    m_offscreen->bind();
}

void App::save_screenshot(int /*width*/, int /*height*/) const {
    const Image image = m_offscreen ? m_offscreen->read_pixels() : m_device->read_screen();
    if (const auto saved = image.save_png(m_config.screenshot); saved) {
        std::println("screenshot saved to {}", m_config.screenshot);
    } else {
        std::println(stderr, "screenshot: {}", saved.error());
    }
}

void App::update_title(Game& game, double fps) {
    const std::string title =
        std::format("{} | tick {} | {} | {:.0f} fps | {} draw calls | {}", m_config.title, m_bus.current_tick(),
                    m_step.paused ? "PAUSED" : std::format("x{}", m_step.speed), fps,
                    m_renderer->last_stats().draw_calls + m_renderer3d->last_stats().draws, game.status());
    m_window.set_title(title);
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

    std::println("memory by tag (MemorySystem::memory_report):\n{}", MemorySystem::memory_report());
    std::println("{:<28} {:>12} {:>10} {:>10} {:>12}", "channel", "emitted", "peak/tick", "dropped", "memory");
    for (std::size_t i = 0; i < m_bus.channel_count(); ++i) {
        const es::IChannel& channel = m_bus.channel_at(i);
        const es::ChannelStats s = channel.stats();
        std::println("{:<28} {:>12} {:>10} {:>10} {:>10} KB", channel.name(), s.total_emitted, s.peak_per_tick,
                     s.total_dropped, s.allocated_bytes / 1024);
    }
}

} // namespace Core
