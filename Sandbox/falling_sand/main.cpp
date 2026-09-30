/**
 * @file main.cpp
 * @brief FallingSand — клеточная симуляция (песок, вода, камень, дерево, огонь).
 *
 * Что показывает:
 * - массовые SoA-события: Simulation отправляет каждое изменение клетки
 *   (`sand.cell_changed`), а View строит по ним своё зеркало мира и читает только колонки;
 * - кисть игрока и «погода» — два независимых производителя одного события `sand.paint`;
 * - память: сетка лежит в арене уровня (MemorySystem, ZII: Material::Empty = 0 — «пусто» без
 *   инициализации), список изменённых за тик клеток — во временной памяти тика (ArenaScope);
 * - ECS: вспышки огня — сущности; анимации всех вспышек обновляются одним вызовом
 *   advance_animations по плотному массиву компонентов;
 * - модули-структуры: свои порты, данные и declare(); чужое приходит в tick() параметрами.
 *
 * Управление: ЛКМ — рисовать, ПКМ — стирать, 1–5 — материал (песок, вода, камень, дерево, огонь),
 * [ / ] — размер кисти. Общие клавиши — см. Core::App.
 */

#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <span>
#include <string_view>

namespace es = EventSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Контракт событий (в движке это был бы публичный заголовок модуля Simulation)
// =============================================================================

/// Материал клетки. Empty = 0: нулевая память — пустой мир (ZII).
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

// =============================================================================
// Сетка
// =============================================================================

struct Grid {
    static constexpr int w = 220;
    static constexpr int h = 124;
    static constexpr std::size_t cells = static_cast<std::size_t>(w) * h;
    static constexpr float cell = 6.0f;

    static std::size_t index(int x, int y) { return static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x); }
    static bool inside(int x, int y) { return x >= 0 && y >= 0 && x < w && y < h; }
};

Color material_color(Material m, int x, int y, std::uint8_t life) {
    // Небольшой шум яркости по координатам, чтобы материал не был плоским.
    const auto hash = static_cast<std::uint32_t>(x * 73856093) ^ static_cast<std::uint32_t>(y * 19349663);
    const float shade = 0.92f + 0.16f * static_cast<float>(hash % 97) / 96.0f;
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
// Модули
// =============================================================================

/// Brush — кисть игрока: материал и радиус; клавиши приходят событием, мышь — опросом кадра.
struct Brush {
    es::EventReader<Core::KeyEvent> keys;
    es::EventWriter<PaintEvent> out;
    Material material = Material::Sand;
    int radius = 3;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Brush")
                                    .consumes<Core::KeyEvent>()
                                    .produces<PaintEvent>(es::ChannelConfig{.reserve = 256, .max_events_per_tick = 2048});
        keys = bus.reader<Core::KeyEvent>(id);
        out = bus.writer<PaintEvent>(id);
    }

    void tick(const Core::FrameInput& input) {
        for (const Core::KeyEvent& key : keys.events()) {
            if (key.action != GLFW_PRESS) continue;
            if (key.key >= GLFW_KEY_1 && key.key <= GLFW_KEY_5) material = static_cast<Material>(key.key - GLFW_KEY_1 + 1);
            if (key.key == GLFW_KEY_LEFT_BRACKET) radius = std::max(radius - 1, 0);
            if (key.key == GLFW_KEY_RIGHT_BRACKET) radius = std::min(radius + 1, 12);
        }
        if (input.down[0] || input.down[1]) {
            out.emit(PaintEvent{.x = static_cast<std::int32_t>(input.mouse_world.x / Grid::cell),
                                .y = static_cast<std::int32_t>(input.mouse_world.y / Grid::cell),
                                .material = static_cast<std::uint32_t>(input.down[0] ? material : Material::Empty),
                                .radius = static_cast<std::uint32_t>(radius)});
        }
    }

    void render(Renderer2D& r, glm::vec2 mouse_world) const {
        const float half = (static_cast<float>(radius) + 0.5f) * Grid::cell;
        r.draw_rect({mouse_world - half, glm::vec2(half * 2.0f)}, 1.5f, material_color(material, 0, 0, 0).with_alpha(200), 10);
    }

    void render_palette(Renderer2D& r) const {
        for (int i = 1; i <= 5; ++i) {
            const Rect slot{{12.0f + static_cast<float>(i - 1) * 30.0f, 12.0f}, {24.0f, 24.0f}};
            r.fill_rect(slot, material_color(static_cast<Material>(i), 0, 0, 0), 1);
            if (static_cast<Material>(i) == material) r.draw_rect({slot.position - 3.0f, slot.size + 6.0f}, 2.0f, Colors::white, 2);
        }
    }
};

/// Weather — демо-активность: источники песка и воды и «молния» по расписанию.
struct Weather {
    struct Source {
        std::int32_t x, y;
        Material material;
        es::Tick period; ///< Каждые period тиков…
        es::Tick phase;  ///< …начиная с этого остатка.
    };
    // Отложенных событий в шине нет, поэтому расписание — таблица по номеру тика.
    static constexpr std::array<Source, 3> sources = {{
        {45, 2, Material::Sand, 1, 0},
        {170, 2, Material::Water, 2, 0},
        {108, 84, Material::Fire, 240, 120}, // «молния» по деревянному блоку каждые 8 секунд
    }};
    es::EventWriter<PaintEvent> out;

    void declare(es::EventBus& bus) { out = bus.writer<PaintEvent>(bus.declare_module("Weather").produces<PaintEvent>()); }

    void tick(es::Tick now) {
        for (const Source& s : sources) {
            if (now % s.period == s.phase) {
                out.emit(PaintEvent{.x = s.x, .y = s.y, .material = static_cast<std::uint32_t>(s.material), .radius = 1});
            }
        }
    }
};

/// Simulation — физика клеток. Сетка в арене уровня; изменения тика — одной пачкой SoA-событий.
struct Simulation {
    es::EventReader<PaintEvent> paint;
    es::EventWriter<CellChangedEvent> changes;
    es::EventWriter<IgnitedEvent> ignited;

    MemorySystem::Arena level = MemorySystem::Arena::reserve(MemorySystem::MiB(4));
    std::span<Material> cells;       ///< Нули = пустой мир (ZII).
    std::span<std::uint8_t> life;    ///< Остаток горения.
    std::span<std::uint32_t> moved;  ///< Номер прохода, в котором клетка уже сдвинулась.
    std::span<std::uint8_t> dirty;   ///< Клетка уже в списке изменений этого тика.
    std::uint32_t stamp = 0;
    std::size_t live_cells = 0;      ///< Непустых клеток; считается по ходу, а не проходом по сетке.
    std::mt19937 rng{12345};

    // Во время тика: список изменённых клеток во временной памяти тика.
    std::uint32_t* dirty_list = nullptr;
    std::size_t dirty_count = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id =
            bus.declare_module("Simulation")
                .consumes<PaintEvent>()
                .produces<CellChangedEvent>(es::ChannelConfig{.reserve = Grid::cells, .max_events_per_tick = Grid::cells})
                .produces<IgnitedEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 256});
        paint = bus.reader<PaintEvent>(id);
        changes = bus.writer<CellChangedEvent>(id);
        ignited = bus.writer<IgnitedEvent>(id);

        cells = level.push_array<Material>(Grid::cells);
        life = level.push_array<std::uint8_t>(Grid::cells);
        moved = level.push_array<std::uint32_t>(Grid::cells);
        dirty = level.push_array<std::uint8_t>(Grid::cells);
    }

    void build_world(MemorySystem::Arena& scratch) {
        begin_changes(scratch);
        for (int x = 0; x < Grid::w; ++x) {
            for (int y = Grid::h - 4; y < Grid::h; ++y) set_cell(x, y, Material::Stone); // пол
        }
        for (int i = 0; i < 40; ++i) set_cell(20 + i, 60 + i / 2, Material::Stone);      // пандус
        for (int i = 0; i < 50; ++i) set_cell(150 + i, 80 - i / 3, Material::Stone);
        for (int y = 85; y < Grid::h - 4; ++y) {                                         // деревянный блок
            for (int x = 95; x < 125; ++x) set_cell(x, y, Material::Wood);
        }
        for (int y = 100; y < Grid::h - 4; ++y) {                                        // бассейн
            set_cell(140, y, Material::Stone);
            set_cell(200, y, Material::Stone);
            for (int x = 141; x < 200; ++x) set_cell(x, y, Material::Water);
        }
        flush_changes();
    }

    void tick(MemorySystem::Arena& scratch) {
        // Список изменений нужен только внутри тика: область откатится и обнулит его на выходе.
        MemorySystem::ArenaScope temporary(scratch);
        begin_changes(scratch);
        apply_paint();
        step_physics();
        flush_changes();
    }

private:
    void begin_changes(MemorySystem::Arena& scratch) {
        dirty_list = scratch.push_array<std::uint32_t>(Grid::cells).data();
        dirty_count = 0;
    }

    void set_cell(int x, int y, Material m, std::uint8_t fire_life = 0) {
        const std::size_t i = Grid::index(x, y);
        if (cells[i] == m && m != Material::Fire) return;
        live_cells += static_cast<std::size_t>(m != Material::Empty) - static_cast<std::size_t>(cells[i] != Material::Empty);
        cells[i] = m;
        life[i] = fire_life;
        mark_dirty(i);
    }

    void mark_dirty(std::size_t i) {
        if (dirty[i] == 0) {
            dirty[i] = 1;
            dirty_list[dirty_count++] = static_cast<std::uint32_t>(i);
        }
    }

    void swap_cells(std::size_t a, std::size_t b) {
        std::swap(cells[a], cells[b]);
        std::swap(life[a], life[b]);
        moved[b] = stamp;
        mark_dirty(a);
        mark_dirty(b);
    }

    [[nodiscard]] bool can_enter(int x, int y, Material mover) const {
        if (!Grid::inside(x, y)) return false;
        const Material target = cells[Grid::index(x, y)];
        return target == Material::Empty || (mover == Material::Sand && target == Material::Water);
    }

    void ignite(int x, int y) {
        set_cell(x, y, Material::Fire, static_cast<std::uint8_t>(std::uniform_int_distribution<int>(25, 50)(rng)));
        ignited.emit(IgnitedEvent{.x = x, .y = y});
    }

    void apply_paint() {
        for (const PaintEvent& p : paint.events()) {
            const auto r = static_cast<int>(p.radius);
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int x = p.x + dx;
                    const int y = p.y + dy;
                    if (dx * dx + dy * dy > r * r + r || !Grid::inside(x, y)) continue;
                    const auto material = static_cast<Material>(p.material);
                    const Material here = cells[Grid::index(x, y)];
                    if (material == Material::Fire) {
                        if (here == Material::Wood || here == Material::Empty) ignite(x, y);
                    } else if (material == Material::Empty || here == Material::Empty) {
                        set_cell(x, y, material);
                    }
                }
            }
        }
    }

    void step_physics() {
        // Снизу вверх, направление строки чередуется, чтобы не было перекоса.
        ++stamp;
        std::uniform_int_distribution<int> coin(0, 1);
        std::uniform_int_distribution<int> percent(0, 99);
        for (int y = Grid::h - 1; y >= 0; --y) {
            const bool left_to_right = (y + static_cast<int>(stamp)) % 2 == 0;
            for (int step = 0; step < Grid::w; ++step) {
                const int x = left_to_right ? step : Grid::w - 1 - step;
                const std::size_t i = Grid::index(x, y);
                if (moved[i] == stamp) continue;

                switch (cells[i]) {
                    case Material::Sand:
                    case Material::Water: {
                        const Material m = cells[i];
                        const int side = coin(rng) == 0 ? -1 : 1;
                        if (can_enter(x, y + 1, m)) {
                            swap_cells(i, Grid::index(x, y + 1));
                        } else if (can_enter(x + side, y + 1, m)) {
                            swap_cells(i, Grid::index(x + side, y + 1));
                        } else if (can_enter(x - side, y + 1, m)) {
                            swap_cells(i, Grid::index(x - side, y + 1));
                        } else if (m == Material::Water) {
                            // Вода растекается вбок на несколько клеток за тик, иначе стоит горкой, как песок.
                            constexpr int dispersion = 8;
                            for (const int dir : {side, -side}) {
                                int reach = 0;
                                while (reach < dispersion && can_enter(x + dir * (reach + 1), y, m)) ++reach;
                                if (reach > 0) {
                                    swap_cells(i, Grid::index(x + dir * reach, y));
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
                            if (!Grid::inside(ax, ay)) continue;
                            const Material n = cells[Grid::index(ax, ay)];
                            if (n == Material::Water) doused = true;
                            if (n == Material::Wood && percent(rng) < 6) ignite(ax, ay);
                        }
                        if (doused || --life[i] == 0) {
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
    }

    /// Все изменения тика — пачкой событий; каждая клетка не больше одного раза.
    void flush_changes() {
        for (std::size_t k = 0; k < dirty_count; ++k) {
            const std::uint32_t i = dirty_list[k];
            changes.emit(CellChangedEvent{.x = static_cast<std::int32_t>(i % Grid::w),
                                          .y = static_cast<std::int32_t>(i / Grid::w),
                                          .material = static_cast<std::uint16_t>(cells[i]), .life = life[i]});
            dirty[i] = 0;
        }
        dirty_list = nullptr;
        dirty_count = 0;
    }
};

/// View — зеркало мира, собранное только из событий: о данных Simulation он не знает.
struct View {
    es::EventReader<CellChangedEvent> changes;
    MemorySystem::Arena memory = MemorySystem::Arena::reserve(MemorySystem::MiB(1));
    std::span<Material> cells;
    std::span<std::uint8_t> life;

    void declare(es::EventBus& bus) {
        changes = bus.reader<CellChangedEvent>(bus.declare_module("View").consumes<CellChangedEvent>());
        cells = memory.push_array<Material>(Grid::cells);
        life = memory.push_array<std::uint8_t>(Grid::cells);
    }

    void tick() {
        // Читаем только колонки — так и задумывалась SoA-раскладка.
        const auto xs = changes.column<&CellChangedEvent::x>();
        const auto ys = changes.column<&CellChangedEvent::y>();
        const auto materials = changes.column<&CellChangedEvent::material>();
        const auto lives = changes.column<&CellChangedEvent::life>();
        for (std::size_t k = 0; k < xs.size(); ++k) {
            const std::size_t i = Grid::index(xs[k], ys[k]);
            cells[i] = static_cast<Material>(materials[k]);
            life[i] = static_cast<std::uint8_t>(lives[k]);
        }
    }

    void render(Renderer2D& r) const {
        for (int y = 0; y < Grid::h; ++y) {
            for (int x = 0; x < Grid::w; ++x) {
                const std::size_t i = Grid::index(x, y);
                if (cells[i] == Material::Empty) continue;
                r.fill_rect({{static_cast<float>(x) * Grid::cell, static_cast<float>(y) * Grid::cell}, {Grid::cell, Grid::cell}},
                            material_color(cells[i], x, y, life[i]), 0);
            }
        }
    }
};

/// Компонент вспышки: где она. Анимация — отдельный компонент AnimationState.
struct Flash {
    glm::vec2 position{0.0f};
};

/// Effects — вспышки огня как сущности ECS.
struct Effects {
    static constexpr std::size_t max_flashes = 96;
    es::EventReader<IgnitedEvent> ignited;
    AnimationLibrary animations;
    ClipId spark;
    TextureHandle texture;

    void declare(es::EventBus& bus) { ignited = bus.reader<IgnitedEvent>(bus.declare_module("Effects").consumes<IgnitedEvent>()); }

    void setup(Renderer2D& renderer) {
        // Процедурный спрайт-лист вспышки 4×1 и незацикленный клип.
        Image sheet(64, 16, Colors::transparent);
        for (int f = 0; f < 4; ++f) {
            const int inset = f * 2;
            sheet.fill_rect(f * 16 + inset, inset, 16 - 2 * inset, 16 - 2 * inset,
                            Color::from_rgba(0xFFF3B0FF).with_alpha(static_cast<std::uint8_t>(230 - f * 50)));
        }
        texture = renderer.create_texture(sheet);
        spark = animations.add(AnimationClip{
            .name = "fx.spark", .frames = make_grid_frames({.columns = 4, .rows = 1}, 0, 4, 0.06f), .looping = false});
    }

    void tick(ECS::World& world, float dt) {
        for (const IgnitedEvent& at : ignited.events()) {
            if (world.count<Flash>() >= max_flashes) break;
            const ECS::Entity e = world.create();
            world.emplace<Flash>(e, glm::vec2{(static_cast<float>(at.x) + 0.5f) * Grid::cell,
                                              (static_cast<float>(at.y) + 0.5f) * Grid::cell});
            world.emplace<AnimationState>(e, AnimationState::start(spark));
        }
        // Все анимации — один вызов по плотному массиву компонентов (раньше — по одной вспышке).
        advance_animations(world.pool<AnimationState>().components(), animations, dt);
        // Доигравшие уничтожаем: удалять текущую сущность во время обхода можно.
        world.view<const Flash, const AnimationState>().each([&](ECS::Entity e, const Flash&, const AnimationState& a) {
            if (is_finished(a, animations)) world.destroy(e);
        });
    }

    void render(const ECS::World& world, Renderer2D& r) const {
        const auto* flashes = world.find_pool<Flash>();
        if (flashes == nullptr) return;
        for (std::size_t k = 0; k < flashes->size(); ++k) {
            const AnimationState* anim = world.get<AnimationState>(flashes->entities()[k]);
            r.draw(SpriteInstance{.position = flashes->components()[k].position, .size = {Grid::cell * 6, Grid::cell * 6},
                                  .uv = current_uv(*anim, animations), .texture = texture, .layer = 5});
        }
    }
};

// =============================================================================
// Игра: хранит модули, вызывает их по порядку, рисует.
// =============================================================================

class FallingSand final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {Grid::w * Grid::cell, Grid::h * Grid::cell}; }

    void setup(Core::App& app) override {
        es::EventBus& bus = app.bus();
        brush.declare(bus);
        weather.declare(bus);
        simulation.declare(bus);
        view.declare(bus);
        effects.declare(bus);

        effects.setup(app.renderer());
        simulation.build_world(app.tick_arena());
    }

    void tick(Core::App& app) override {
        brush.tick(app.input());
        weather.tick(app.tick());
        simulation.tick(app.tick_arena());
        view.tick();
        effects.tick(world, app.tick_seconds());
    }

    void render(Core::App& app, Renderer2D& r) override {
        r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x0E1016FF), -10);
        view.render(r);
        effects.render(world, r);
        brush.render(r, app.input().mouse_world);
    }

    void render_overlay(Core::App& /*app*/, Renderer2D& r) override { brush.render_palette(r); }

    [[nodiscard]] std::string status() const override {
        return std::format("cells {} | changes/tick {} | flashes {}", simulation.live_cells, view.changes.size(),
                           world.count<Flash>());
    }

private:
    ECS::World world;
    Brush brush;
    Weather weather;
    Simulation simulation;
    View view;
    Effects effects;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<FallingSand>({.title = "FallingSand", .ticks_per_second = 30.0}, argc, argv);
}
