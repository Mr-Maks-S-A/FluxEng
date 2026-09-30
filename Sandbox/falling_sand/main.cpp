/**
 * @file main.cpp
 * @brief FallingSand — клеточная симуляция (песок, вода, камень, дерево, огонь).
 *
 * Что показывает:
 * - массовые SoA-события: Simulation отправляет каждое изменение клетки
 *   (`sand.cell_changed`), а View строит по ним своё зеркало мира и читает только колонки;
 * - кисть игрока и «погода» — два независимых производителя одного события `sand.paint`;
 * - Effects реагирует на `sand.ignited` вспышками с покадровой анимацией;
 * - ввод приходит из шины (`platform.key`), а не из колбэков окна.
 *
 * Управление: ЛКМ — рисовать, ПКМ — стирать, 1–5 — материал (песок, вода, камень, дерево, огонь),
 * [ / ] — размер кисти. Общие клавиши — см. Core::App.
 */

#include <Core/Core.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <string_view>
#include <vector>

namespace es = EventSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Контракт событий (в движке это был бы публичный заголовок модуля Simulation)
// =============================================================================

enum class Material : std::uint8_t { Empty, Sand, Water, Stone, Wood, Fire };

/// Закрасить круг клеток материалом.
struct PaintEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t material = 0;
    std::uint32_t radius = 0;

    static constexpr std::string_view event_name = "sand.paint";
    using fields = es::Fields<es::Field<"x", &PaintEvent::x>, es::Field<"y", &PaintEvent::y>,
                              es::Field<"material", &PaintEvent::material>,
                              es::Field<"radius", &PaintEvent::radius>>;
};

/// Клетка сменила материал. Массовое событие — раскладка SoA.
struct CellChangedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint16_t material = 0;
    std::uint16_t life = 0; ///< Остаток горения (для цвета огня).

    static constexpr std::string_view event_name = "sand.cell_changed";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<es::Field<"x", &CellChangedEvent::x>, es::Field<"y", &CellChangedEvent::y>,
                              es::Field<"material", &CellChangedEvent::material>,
                              es::Field<"life", &CellChangedEvent::life>>;
};

/// Дерево загорелось.
struct IgnitedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;

    static constexpr std::string_view event_name = "sand.ignited";
    using fields = es::Fields<es::Field<"x", &IgnitedEvent::x>, es::Field<"y", &IgnitedEvent::y>>;
};

constexpr int grid_w = 220;
constexpr int grid_h = 124;
constexpr float cell_size = 6.0f;

Color material_color(Material m, int x, int y, std::uint8_t life) {
    // Небольшой шум яркости по координатам, чтобы материал не был плоским.
    const auto h = static_cast<std::uint32_t>(x * 73856093) ^ static_cast<std::uint32_t>(y * 19349663);
    const float shade = 0.92f + 0.16f * static_cast<float>(h % 97) / 96.0f;
    auto tint = [shade](std::uint32_t rgba) {
        const glm::vec4 c = Color::from_rgba(rgba).to_vec4();
        return Color::from_floats(c.r * shade, c.g * shade, c.b * shade, c.a);
    };
    switch (m) {
        case Material::Sand: return tint(0xE3C07AFF);
        case Material::Water: return tint(0x3A7BD5D0);
        case Material::Stone: return tint(0x74747CFF);
        case Material::Wood: return tint(0x8B5A2BFF);
        case Material::Fire: return (life / 4) % 2 == 0 ? Color::from_rgba(0xFF7A1AFF) : Color::from_rgba(0xFFD23FFF);
        case Material::Empty: break;
    }
    return Colors::transparent;
}

// =============================================================================

class FallingSand final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {grid_w * cell_size, grid_h * cell_size}; }

    void setup(Core::App& app) override {
        es::EventBus& bus = app.bus();

        // ---- контракты модулей: это и есть граф, который App печатает на старте
        const es::ModuleId brush = bus.declare_module("Brush")
                                       .consumes<Core::KeyEvent>()
                                       .produces<PaintEvent>(es::ChannelConfig{.reserve = 256, .max_events_per_tick = 2048});
        const es::ModuleId weather = bus.declare_module("Weather").produces<PaintEvent>();
        const es::ModuleId simulation =
            bus.declare_module("Simulation")
                .consumes<PaintEvent>()
                .produces<CellChangedEvent>(es::ChannelConfig{.reserve = grid_w * grid_h,
                                                              .max_events_per_tick = grid_w * grid_h})
                .produces<IgnitedEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 256});
        const es::ModuleId view = bus.declare_module("View").consumes<CellChangedEvent>();
        const es::ModuleId effects = bus.declare_module("Effects").consumes<IgnitedEvent>();

        m_brush_keys = bus.reader<Core::KeyEvent>(brush);
        m_brush_out = bus.writer<PaintEvent>(brush);
        m_weather_out = bus.writer<PaintEvent>(weather);
        m_sim_paint = bus.reader<PaintEvent>(simulation);
        m_sim_changes = bus.writer<CellChangedEvent>(simulation);
        m_sim_ignited = bus.writer<IgnitedEvent>(simulation);
        m_view_changes = bus.reader<CellChangedEvent>(view);
        m_effects_ignited = bus.reader<IgnitedEvent>(effects);

        // ---- данные
        m_cells.assign(grid_w * grid_h, Material::Empty);
        m_life.assign(grid_w * grid_h, 0);
        m_moved.assign(grid_w * grid_h, 0);
        m_dirty.assign(grid_w * grid_h, 0);
        m_view.assign(grid_w * grid_h, Material::Empty);
        m_view_life.assign(grid_w * grid_h, 0);
        build_initial_world();

        // ---- эффекты: процедурный спрайт-лист вспышки 4×1 и незацикленный клип
        Image sheet(64, 16, Colors::transparent);
        for (int f = 0; f < 4; ++f) {
            const int inset = f * 2;
            sheet.fill_rect(f * 16 + inset, inset, 16 - 2 * inset, 16 - 2 * inset,
                            Color::from_rgba(0xFFF3B0FF).with_alpha(static_cast<std::uint8_t>(230 - f * 50)));
        }
        m_spark_texture = app.renderer().create_texture(sheet);
        m_spark_clip = m_animations.add(AnimationClip{
            .name = "fx.spark", .frames = make_grid_frames({.columns = 4, .rows = 1}, 0, 4, 0.06f), .looping = false});
    }

    void tick(Core::App& app) override {
        tick_brush(app);
        tick_weather(app);
        tick_simulation();
        tick_view();
        tick_effects(app);
    }

    void render(Core::App& app, Renderer2D& r) override {
        r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x0E1016FF), -10);

        // View рисует своё зеркало мира, собранное только из событий.
        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const std::size_t i = index(x, y);
                if (m_view[i] != Material::Empty) {
                    r.fill_rect({{static_cast<float>(x) * cell_size, static_cast<float>(y) * cell_size},
                                 {cell_size, cell_size}},
                                material_color(m_view[i], x, y, m_view_life[i]), 0);
                }
            }
        }

        for (const Flash& flash : m_flashes) {
            r.draw(SpriteInstance{.position = flash.position, .size = {cell_size * 6, cell_size * 6},
                                  .uv = current_uv(flash.anim, m_animations), .texture = m_spark_texture, .layer = 5});
        }

        // Контур кисти.
        const glm::vec2 m = app.input().mouse_world;
        const float half = (static_cast<float>(m_radius) + 0.5f) * cell_size;
        r.draw_rect({m - half, glm::vec2(half * 2.0f)}, 1.5f, material_color(m_brush, 0, 0, 0).with_alpha(200), 10);
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        // Палитра материалов слева сверху; выбранный — в рамке.
        for (int i = 1; i <= 5; ++i) {
            const Rect slot{{12.0f + static_cast<float>(i - 1) * 30.0f, 12.0f}, {24.0f, 24.0f}};
            r.fill_rect(slot, material_color(static_cast<Material>(i), 0, 0, 0), 1);
            if (static_cast<Material>(i) == m_brush) {
                r.draw_rect({slot.position - 3.0f, slot.size + 6.0f}, 2.0f, Colors::white, 2);
            }
        }
        (void)app;
    }

    [[nodiscard]] std::string status() const override {
        return std::format("cells {} | changes/tick {} | flashes {}", m_live_cells, m_view_changes.size(),
                           m_flashes.size());
    }

private:
    struct Flash {
        glm::vec2 position;
        AnimationState anim;
    };

    static std::size_t index(int x, int y) { return static_cast<std::size_t>(y) * grid_w + static_cast<std::size_t>(x); }
    static bool inside(int x, int y) { return x >= 0 && y >= 0 && x < grid_w && y < grid_h; }

    // ------------------------------------------------------------------ Brush

    void tick_brush(Core::App& app) {
        for (const Core::KeyEvent& key : m_brush_keys.events()) {
            if (key.action != GLFW_PRESS) continue;
            if (key.key >= GLFW_KEY_1 && key.key <= GLFW_KEY_5) m_brush = static_cast<Material>(key.key - GLFW_KEY_1 + 1);
            if (key.key == GLFW_KEY_LEFT_BRACKET) m_radius = std::max(m_radius - 1, 0);
            if (key.key == GLFW_KEY_RIGHT_BRACKET) m_radius = std::min(m_radius + 1, 12);
        }
        const Core::FrameInput& in = app.input();
        if (in.down[0] || in.down[1]) {
            const auto x = static_cast<std::int32_t>(in.mouse_world.x / cell_size);
            const auto y = static_cast<std::int32_t>(in.mouse_world.y / cell_size);
            m_brush_out.emit(PaintEvent{.x = x, .y = y,
                                        .material = static_cast<std::uint32_t>(in.down[0] ? m_brush : Material::Empty),
                                        .radius = static_cast<std::uint32_t>(m_radius)});
        }
    }

    // ------------------------------------------------------------------ Weather (демо-активность)

    void tick_weather(Core::App& app) {
        const es::Tick tick = app.tick();
        m_weather_out.emit(PaintEvent{.x = 45, .y = 2, .material = static_cast<std::uint32_t>(Material::Sand), .radius = 1});
        if (tick % 2 == 0) {
            m_weather_out.emit(PaintEvent{.x = 170, .y = 2, .material = static_cast<std::uint32_t>(Material::Water), .radius = 1});
        }
        // «Молния» по деревянному блоку каждые 8 секунд. Отложенных событий в шине нет,
        // поэтому расписание ведётся вручную по номеру тика.
        if (tick % 240 == 120) {
            m_weather_out.emit(PaintEvent{.x = 108, .y = 84, .material = static_cast<std::uint32_t>(Material::Fire), .radius = 1});
        }
    }

    // ------------------------------------------------------------------ Simulation

    void set_cell(int x, int y, Material m, std::uint8_t life = 0) {
        const std::size_t i = index(x, y);
        if (m_cells[i] == m && m != Material::Fire) return;
        m_cells[i] = m;
        m_life[i] = life;
        mark_dirty(i);
    }

    void mark_dirty(std::size_t i) {
        if (m_dirty[i] == 0) {
            m_dirty[i] = 1;
            m_dirty_list.push_back(static_cast<std::uint32_t>(i));
        }
    }

    void swap_cells(std::size_t a, std::size_t b) {
        std::swap(m_cells[a], m_cells[b]);
        std::swap(m_life[a], m_life[b]);
        m_moved[b] = m_stamp;
        mark_dirty(a);
        mark_dirty(b);
    }

    bool can_enter(int x, int y, Material mover) const {
        if (!inside(x, y)) return false;
        const Material target = m_cells[index(x, y)];
        return target == Material::Empty || (mover == Material::Sand && target == Material::Water);
    }

    void ignite(int x, int y) {
        set_cell(x, y, Material::Fire, static_cast<std::uint8_t>(std::uniform_int_distribution<int>(25, 50)(m_rng)));
        m_sim_ignited.emit(IgnitedEvent{.x = x, .y = y});
    }

    void tick_simulation() {
        // 1. Команды рисования от кисти и погоды.
        for (const PaintEvent& paint : m_sim_paint.events()) {
            const auto r = static_cast<int>(paint.radius);
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int x = paint.x + dx;
                    const int y = paint.y + dy;
                    if (dx * dx + dy * dy > r * r + r || !inside(x, y)) continue;
                    const auto material = static_cast<Material>(paint.material);
                    if (material == Material::Fire) {
                        if (m_cells[index(x, y)] == Material::Wood || m_cells[index(x, y)] == Material::Empty) ignite(x, y);
                    } else if (material == Material::Empty || m_cells[index(x, y)] == Material::Empty) {
                        set_cell(x, y, material);
                    }
                }
            }
        }

        // 2. Физика: снизу вверх, направление строки чередуется, чтобы не было перекоса.
        ++m_stamp;
        std::uniform_int_distribution<int> coin(0, 1);
        std::uniform_int_distribution<int> percent(0, 99);
        for (int y = grid_h - 1; y >= 0; --y) {
            const bool left_to_right = (y + static_cast<int>(m_stamp)) % 2 == 0;
            for (int step = 0; step < grid_w; ++step) {
                const int x = left_to_right ? step : grid_w - 1 - step;
                const std::size_t i = index(x, y);
                if (m_moved[i] == m_stamp) continue;

                switch (m_cells[i]) {
                    case Material::Sand:
                    case Material::Water: {
                        const Material m = m_cells[i];
                        const int side = coin(m_rng) == 0 ? -1 : 1;
                        if (can_enter(x, y + 1, m)) {
                            swap_cells(i, index(x, y + 1));
                        } else if (can_enter(x + side, y + 1, m)) {
                            swap_cells(i, index(x + side, y + 1));
                        } else if (can_enter(x - side, y + 1, m)) {
                            swap_cells(i, index(x - side, y + 1));
                        } else if (m == Material::Water) {
                            // Вода растекается вбок на несколько клеток за тик, иначе стоит горкой, как песок.
                            constexpr int dispersion = 8;
                            for (const int dir : {side, -side}) {
                                int reach = 0;
                                while (reach < dispersion && can_enter(x + dir * (reach + 1), y, m)) ++reach;
                                if (reach > 0) {
                                    swap_cells(i, index(x + dir * reach, y));
                                    break;
                                }
                            }
                        }
                        break;
                    }
                    case Material::Fire: {
                        constexpr int nx[] = {1, -1, 0, 0};
                        constexpr int ny[] = {0, 0, 1, -1};
                        bool doused = false;
                        for (int k = 0; k < 4; ++k) {
                            const int ax = x + nx[k];
                            const int ay = y + ny[k];
                            if (!inside(ax, ay)) continue;
                            const Material n = m_cells[index(ax, ay)];
                            if (n == Material::Water) doused = true;
                            if (n == Material::Wood && percent(m_rng) < 6) ignite(ax, ay);
                        }
                        if (doused || --m_life[i] == 0) {
                            set_cell(x, y, Material::Empty);
                        } else {
                            mark_dirty(i); // мерцание: цвет огня зависит от life
                        }
                        break;
                    }
                    default: break;
                }
            }
        }

        // 3. Все изменения тика — пачкой событий; каждая клетка не больше одного раза.
        m_live_cells = 0;
        for (const Material m : m_cells) m_live_cells += m != Material::Empty ? 1u : 0u;
        for (const std::uint32_t i : m_dirty_list) {
            m_sim_changes.emit(CellChangedEvent{.x = static_cast<std::int32_t>(i % grid_w),
                                                .y = static_cast<std::int32_t>(i / grid_w),
                                                .material = static_cast<std::uint16_t>(m_cells[i]),
                                                .life = m_life[i]});
            m_dirty[i] = 0;
        }
        m_dirty_list.clear();
    }

    void build_initial_world() {
        for (int x = 0; x < grid_w; ++x) {
            for (int y = grid_h - 4; y < grid_h; ++y) set_cell(x, y, Material::Stone);  // пол
        }
        for (int i = 0; i < 40; ++i) set_cell(20 + i, 60 + i / 2, Material::Stone);      // пандус
        for (int i = 0; i < 50; ++i) set_cell(150 + i, 80 - i / 3, Material::Stone);
        for (int y = 85; y < grid_h - 4; ++y) {                                          // деревянный блок
            for (int x = 95; x < 125; ++x) set_cell(x, y, Material::Wood);
        }
        for (int y = 100; y < grid_h - 4; ++y) {                                         // бассейн
            set_cell(140, y, Material::Stone);
            set_cell(200, y, Material::Stone);
            for (int x = 141; x < 200; ++x) set_cell(x, y, Material::Water);
        }
    }

    // ------------------------------------------------------------------ View

    void tick_view() {
        // Читаем только колонки — так и задумывалась SoA-раскладка.
        const auto xs = m_view_changes.column<&CellChangedEvent::x>();
        const auto ys = m_view_changes.column<&CellChangedEvent::y>();
        const auto materials = m_view_changes.column<&CellChangedEvent::material>();
        const auto lives = m_view_changes.column<&CellChangedEvent::life>();
        for (std::size_t k = 0; k < xs.size(); ++k) {
            const std::size_t i = index(xs[k], ys[k]);
            m_view[i] = static_cast<Material>(materials[k]);
            m_view_life[i] = static_cast<std::uint8_t>(lives[k]);
        }
    }

    // ------------------------------------------------------------------ Effects

    void tick_effects(Core::App& app) {
        for (const IgnitedEvent& ignition : m_effects_ignited.events()) {
            if (m_flashes.size() >= 96) break;
            m_flashes.push_back(Flash{
                .position = {(static_cast<float>(ignition.x) + 0.5f) * cell_size,
                             (static_cast<float>(ignition.y) + 0.5f) * cell_size},
                .anim = AnimationState::start(m_spark_clip),
            });
        }
        // Состояния анимаций лежат внутри Flash, а advance_animations принимает span<AnimationState>:
        // для AoS-данных приходится обновлять по одному (см. README, раздел «Что дорабатывать»).
        for (Flash& flash : m_flashes) {
            advance_animations(std::span(&flash.anim, 1), m_animations, app.tick_seconds());
        }
        std::erase_if(m_flashes, [&](const Flash& f) { return is_finished(f.anim, m_animations); });
    }

    // ---- писатели и читатели модулей
    es::EventReader<Core::KeyEvent> m_brush_keys;
    es::EventWriter<PaintEvent> m_brush_out;
    es::EventWriter<PaintEvent> m_weather_out;
    es::EventReader<PaintEvent> m_sim_paint;
    es::EventWriter<CellChangedEvent> m_sim_changes;
    es::EventWriter<IgnitedEvent> m_sim_ignited;
    es::EventReader<CellChangedEvent> m_view_changes;
    es::EventReader<IgnitedEvent> m_effects_ignited;

    // ---- Brush
    Material m_brush = Material::Sand;
    int m_radius = 3;

    // ---- Simulation
    std::vector<Material> m_cells;
    std::vector<std::uint8_t> m_life;
    std::vector<std::uint32_t> m_moved;
    std::vector<std::uint8_t> m_dirty;
    std::vector<std::uint32_t> m_dirty_list;
    std::uint32_t m_stamp = 0;
    std::size_t m_live_cells = 0;
    std::mt19937 m_rng{12345};

    // ---- View
    std::vector<Material> m_view;
    std::vector<std::uint8_t> m_view_life;

    // ---- Effects
    AnimationLibrary m_animations;
    ClipId m_spark_clip;
    TextureHandle m_spark_texture;
    std::vector<Flash> m_flashes;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<FallingSand>({.title = "FallingSand", .ticks_per_second = 30.0}, argc, argv);
}
