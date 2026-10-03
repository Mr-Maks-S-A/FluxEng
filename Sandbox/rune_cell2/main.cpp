/**
 * @file main.cpp
 * @brief RuneCell2 — заклинание, которое вы рисуете сами: вид сверху на SDF-мир, поле маны под ногами и визуальный редактор графа рун.
 *
 * Игра — склейка модулей движка, своей логики почти нет:
 * - **SpellSim** — мир и порядок тика (команды → заклинания → ландшафт → мана → движение → события);
 * - **RuneEditor** — редактор графа рун: правки как данные, undo/redo, живая диагностика, жесты мыши, автосохранение;
 * - **Runes** — граф → байт-код; карта «руна → узел» подсвечивает в редакторе то, что только что исполнилось;
 * - **WorldRender::HeightMap** — карта высот вида сверху (пересчёт только изменённых чанков), поле маны — тепловым слоем;
 * - **Replay** + **EventLog** — запись/повтор (`--record`, `--replay`) и журнал автосохранения редактора с избыточностью;
 * - **Core** — окно, тик 60 Гц, ввод через действия.
 *
 * Идея игры: заклинание — это граф. Вы строите его на экране (руны-круги, провода данных и управления), он на лету
 * собирается в байт-код и подставляется в слот 1. Выстрел показывает, какие узлы сработали, сколько маны ушло и что
 * стало с миром. Мана — физическое поле: из окружающей маны для мага в 10 раз дешевле, но дыра в поле затягивается медленно.
 *
 * Управление мира: WASD — ходьба, Пробел — прыжок (пауза — P), ЛКМ — каст из личного запаса маны в точку под курсором,
 * ПКМ — из окружающей, 1–3 — слот гримуара (слот 1 — ваш граф «edit»), Tab — редактор.
 * Редактор: перетащите узел — переместить; потяните от порта — ребро; потяните занятый вход или ребро — перенести (в пустоту — удалить);
 * рамка — выделение; ПКМ — сдвиг; колесо — масштаб (над узлом PUSH — значение); палитра рун сверху — клик ставит узел;
 * Del/X — удалить выбранное, Ctrl+Z / Ctrl+Y — undo/redo, L — раскладка, F — вписать, E — сделать выбранный оператор входом,
 * C — отменить жест, Enter — выстрел из слота 1 из редактора, F5 — сохранить граф в файл.
 * Аргументы: `--spell файл.rungraph` (стартовый граф; по умолчанию spells2/carve.rungraph), `--recover` (взять граф из автосохранения), `--editor` (сразу открыть редактор),
 * `--autosave файл`, `--seed N`, `--record файл`, `--replay файл` (редактор в них только для чтения: граф — часть настройки, а не команда),
 * `--autoplay` (сценарий без ввода с проверками), общие (`--ticks`, `--threads`, `--screenshot`, `--backend`) — см. Core::App.
 */

#include <Core/Core.hpp>
#include <RuneEditor/Analysis.hpp>
#include <RuneEditor/Autosave.hpp>
#include <RuneEditor/Layout.hpp>
#include <RuneEditor/View.hpp>
#include <SpellSim/SpellSim.hpp>
#include <WorldRender/Adapters/Terrain.hpp>
#include <WorldRender/WorldRender.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <sstream>
#include <utility>

#ifndef RUNE_CELL2_SPELLS_DIR
#define RUNE_CELL2_SPELLS_DIR "spells2"
#endif

namespace es = EventSystem;
using namespace RendererSystem;
using Math::Fixed;
namespace overlay = WorldRender::overlay;
namespace ed = RuneEditor;
using Runes::Rune;

namespace {

int g_failed_checks = 0; ///< Автоигра: число проваленных проверок (код возврата).

constexpr const char* edit_program = "edit"; ///< Имя программы слота 1 в библиотеке: её подменяет редактор.
constexpr float view_meters = 56.0f;         ///< Сколько метров мира по высоте экрана.

Color mix(Color a, Color b, float t) {
    const auto lerp = [&](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t); };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

struct Actions {
    Core::ActionMap map;
    Core::ActionId forward, back, left, right, jump, cast_personal, cast_ambient, toggle_editor;
    std::array<Core::ActionId, Character::Grimoire::slot_count> slot;

    Actions() {
        using Core::Binding;
        forward = map.declare("move_up", "Вверх по карте");
        back = map.declare("move_down", "Вниз по карте");
        left = map.declare("move_left", "Влево");
        right = map.declare("move_right", "Вправо");
        jump = map.declare("jump", "Прыжок");
        cast_personal = map.declare("cast_personal", "Каст из личного запаса маны");
        cast_ambient = map.declare("cast_ambient", "Каст из окружающей маны");
        toggle_editor = map.declare("toggle_editor", "Редактор рун");
        for (std::size_t i = 0; i < slot.size(); ++i) slot[i] = map.declare("slot_" + std::to_string(i + 1), "Слот гримуара " + std::to_string(i + 1));
        map.bind(forward, Binding::key(GLFW_KEY_W)).bind(back, Binding::key(GLFW_KEY_S)).bind(left, Binding::key(GLFW_KEY_A)).bind(right, Binding::key(GLFW_KEY_D));
        map.bind(jump, Binding::key(GLFW_KEY_SPACE));
        map.bind(cast_personal, Binding::mouse(GLFW_MOUSE_BUTTON_LEFT)).bind(cast_ambient, Binding::mouse(GLFW_MOUSE_BUTTON_RIGHT));
        map.bind(toggle_editor, Binding::key(GLFW_KEY_TAB));
        for (std::size_t i = 0; i < slot.size(); ++i) map.bind(slot[i], Binding::key(GLFW_KEY_1 + static_cast<int>(i)));
    }
};

/// Палитра рун в редакторе: что можно поставить.
constexpr std::array<Rune, 12> palette{Rune::Push, Rune::Add, Rune::Mul, Rune::Caster, Rune::Aim, Rune::Target, Rune::ManaAt, Rune::JmpIf, Rune::Halt, Rune::Draw, Rune::Carve, Rune::Raise};
constexpr float palette_height = 34.0f;

class RuneCell2 final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {1280.0f, 720.0f}; }

    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        const auto arg = [&](std::string_view name, std::string fallback) {
            for (std::size_t i = 0; i + 1 < args.size(); ++i) {
                if (args[i] == name) return args[i + 1];
            }
            return fallback;
        };
        autoplay = std::ranges::find(args, "--autoplay") != args.end();
        editor_open = std::ranges::find(args, "--editor") != args.end();
        spell_path = arg("--spell", std::string(RUNE_CELL2_SPELLS_DIR) + "/carve.rungraph");
        const std::string autosave_path = arg("--autosave", autoplay ? "rune_cell2_autoplay.fluxlog" : "rune_cell2_autosave.fluxlog");

        SpellSim::register_commands(command_registry);
        auto parsed = Replay::Session::from_args(args, 1, &command_registry);
        if (!parsed) throw std::runtime_error(parsed.error());
        session.emplace(std::move(*parsed));
        locked = session->mode() != Replay::Session::Mode::Off;

        // Слот 1 — программа «edit»: её содержимое всегда последний собравшийся граф редактора.
        SpellSim::Config config{.seed = session->seed()};
        config.grimoire = {edit_program, "mound", "siphon"};
        sim = std::make_unique<SpellSim::Simulation>(config);
        sim->declare(app.bus());
        const es::ModuleId me = app.bus().declare_module("RuneCell2").consumes<Runes::SpellFailedEvent>();
        failures = app.bus().reader<Runes::SpellFailedEvent>(me);
        sim->reload_spells(RUNE_CELL2_SPELLS_DIR); // готовые графы каталога: слоты 2–3 (mound, siphon)
        recorder.install_assert_dump("flight_recorder_rune_cell2.txt");
        driver.emplace(*sim, *session, &recorder);

        load_start_graph(std::ranges::find(args, "--recover") != args.end() ? autosave_path : std::string{});
        refresh();
        // Автосохранение редактора — журнал с избыточностью. В записи/повторе правок нет — нечего сохранять.
        if (!locked) {
            storage = EventLog::FileStorage::open(autosave_path, EventLog::FileStorage::Mode::Create);
            if (storage) {
                if (auto started = ed::Autosave::start(*storage, editor)) autosave.emplace(std::move(*started));
                else std::println(stderr, "autosave: {}", started.error());
            }
        }

        // Карта высот и слой маны — текстуры, обновляемые на лету.
        heights.emplace(256, 256, 0.5); // клетка 0,5 м: рельеф резче
        height_source.emplace(sim->terrain());
        heights->update(*height_source);
        height_tex = app.renderer().create_texture(shaded(), {.filter = TextureFilter::Linear});
        mana_tex = app.renderer().create_texture(Image(64, 64, Colors::transparent), {.filter = TextureFilter::Linear});
        disc = app.renderer().create_texture(Procedural::circle_image(64, Colors::white), {.filter = TextureFilter::Linear});
        ring = app.renderer().create_texture(Procedural::circle_image(64, Colors::white, 3.0f), {.filter = TextureFilter::Linear});
        editor_view.emplace(app.renderer());
        (void)sim->take_dirty_chunks(); // стартовый мир уже учтён целиком
        std::println("RuneCell2: seed {} | {} | граф: {} узлов{} | автосохранение: {}", session->seed(),
                     locked ? (session->mode() == Replay::Session::Mode::Replay ? "REPLAY" : "RECORD") : "live", editor.graph().nodes().size(),
                     analysis.ok() ? ", собран" : ", НЕ собран", autosave ? autosave_path : "выкл");
    }

    // ---------------------------------------------------------------- кадр: ввод, редактор, обновление карт

    void frame(Core::App& app, float seconds) override {
        ++frames;
        const auto& in = app.window().input();
        const Core::ActionMap& act = actions.map;
        const glm::vec2 vp = app.camera().viewport;
        const WindowSystem::Vec2d cur = app.window().cursor_in_framebuffer();
        cursor = {static_cast<float>(cur.x), static_cast<float>(cur.y)};

        if (act.pressed(in, actions.toggle_editor)) editor_open = !editor_open;
        for (int i = 0; i < Character::Grimoire::slot_count; ++i) {
            if (act.pressed(in, actions.slot[static_cast<std::size_t>(i)])) slot = i;
        }
        for (auto it = glow.begin(); it != glow.end();) { // свечение исполнения затухает за ~1 с
            it->second -= seconds * 1.1f;
            it = it->second <= 0.0f ? glow.erase(it) : std::next(it);
        }

        if (editor_open) editor_input(in, vp);
        else world_input(in, act);
        if (!autoplay) refresh();

        // Карты: высоты — по чанкам, поменявшимся в тике; мана — раз в шаг поля.
        if (heights_dirty) {
            app.renderer().update_texture(height_tex, shaded());
            heights_dirty = false;
        }
        if (sim->tick_number() != mana_drawn_tick && sim->tick_number() % SpellSim::mana_step_period == 0) {
            mana_drawn_tick = sim->tick_number();
            app.renderer().update_texture(mana_tex, mana_image());
        }
    }

    // ------------------------------------------------------------------------ тик: команды → симуляция

    void tick(Core::App& app) override {
        const std::uint32_t t = sim->tick_number();
        live.clear();
        if (!driver->replaying()) {
            if (autoplay) gather_autoplay(t);
            else gather_live();
        }
        const std::uint64_t casts_before = sim->casts();
        if (!driver->step(live)) {
            app.window().request_close();
            return;
        }
        for (const Terrain::ChunkCoord c : sim->take_dirty_chunks()) {
            if (heights->update(*height_source, WorldRender::TerrainHeights::cells_of_chunk(c, *heights)) > 0) heights_dirty = true;
        }
        if (sim->casts() != casts_before) light_up_trace();
        for (const Runes::SpellFailedEvent& f : failures.events()) {
            last_failure = std::format("spell failed: {} (rune {})", Runes::failure_text(static_cast<Runes::Failure>(f.reason)), f.pc);
            failure_until = t + 60 * 4;
        }
        if (autosave && t % 120 == 119 && autosave->operations_logged() != flushed_ops) {
            autosave->flush();
            flushed_ops = autosave->operations_logged();
            if (flushed_ops > 400) autosave->compact(), flushed_ops = autosave->operations_logged();
        }
    }

    // ------------------------------------------------------------------------------------ рисование

    void render_overlay(Core::App& app, Renderer2D& r) override {
        const glm::vec2 vp = app.camera().viewport;
        const FontHandle font = app.ui_font();
        const glm::dvec2 centre = player_xz(app.tick_alpha());
        const float ppm = vp.y / view_meters;
        const auto to_screen = [&](double x, double z) { return glm::vec2{vp.x * 0.5f + static_cast<float>(x - centre.x) * ppm, vp.y * 0.5f + static_cast<float>(z - centre.y) * ppm}; };
        const glm::vec2 origin = to_screen(0.0, 0.0);
        const float world_px = 128.0f * ppm;

        r.fill_rect({{0, 0}, vp}, Color{10, 12, 22, 255}, -10);
        r.draw({.position = origin, .size = {world_px, world_px}, .pivot = {0, 0}, .texture = height_tex, .layer = -5});
        r.draw({.position = origin, .size = {world_px, world_px}, .pivot = {0, 0}, .texture = mana_tex, .layer = -4});
        r.draw_rect({origin, {world_px, world_px}}, 2.0f, Color{255, 255, 255, 60}, -3);

        // Персонаж, прицел и дальность прыжка по линии «персонаж → курсор».
        const glm::vec2 me = to_screen(centre.x, centre.y);
        const glm::vec2 aim = cursor;
        if (!editor_open) {
            r.draw_line(me, aim, 1.5f, Color{255, 240, 160, 110}, -2);
            r.draw({.position = aim, .size = {16, 16}, .color = Color{255, 240, 160, 200}, .texture = ring, .layer = -2});
        }
        r.draw({.position = me, .size = {ppm * 1.4f, ppm * 1.4f}, .color = Color{20, 20, 40, 255}, .texture = disc, .layer = -1});
        r.draw({.position = me, .size = {ppm * 1.1f, ppm * 1.1f}, .color = Color{150, 130, 255, 255}, .texture = disc, .layer = 0});

        draw_hud(app, r, font, vp);

        if (editor_open) {
            const ed::Marks marks{.activity = &glow, .analysis = &analysis, .locked = locked};
            const Rect area = editor_area(vp);
            editor_view->draw(r, font, editor, controller, marks, area, 20);
            draw_palette(r, font, area);
        }
    }

    [[nodiscard]] std::string status() const override {
        return std::format("tick {} | mana {:.0f}", sim ? sim->tick_number() : 0, sim ? sim->world().get<Character::ManaPool>(sim->player())->current.to_double() : 0.0);
    }

    void shutdown(Core::App& app) override {
        const std::uint32_t ticks = sim->tick_number();
        std::println("\n===== RuneCell2: {} ticks, {} frames =====", ticks, frames);
        std::println("hashes: {}", sim->hashes().describe());
        std::println("casts {} (empty slot {}), terrain edits {}, editor ops logged {}", sim->casts(), sim->failed_casts(), sim->edits_applied(), autosave ? autosave->operations_logged() : 0);
        if (autoplay) finish_autoplay();
        if (auto verdict = driver->finish()) {
            if (const std::string text = Replay::describe(*verdict, session->mode(), ticks, session->recording().command_count()); !text.empty()) std::println("{}", text);
        } else {
            std::println(stderr, "replay: {}", verdict.error());
        }
        recorder.uninstall_assert_dump();
        autosave.reset();
        (void)app;
    }

private:
    /// Карта высот с раскраской по фактическому диапазону: рельеф виден при любой высоте мира.
    [[nodiscard]] Image shaded() const {
        const auto [low, high] = heights->range();
        return heights->shade({.exaggeration = 2.5, .low = low, .high = high});
    }

    // ================================================================ редактор

    [[nodiscard]] Rect editor_area(glm::vec2 vp) const { return {{12.0f, 12.0f}, {vp.x - 24.0f, vp.y - 24.0f}}; }

    void load_start_graph(const std::string& recover_from) {
        if (!recover_from.empty()) {
            if (auto file = EventLog::FileStorage::open(recover_from, EventLog::FileStorage::Mode::Existing)) {
                if (auto got = ed::Autosave::recover(*file)) {
                    editor.reset(std::move(got->graph));
                    std::println("autosave: восстановлено ({} правок после снимка, блоков восстановлено по чётности: {})", got->operations, got->report.blocks_repaired);
                    return;
                } else {
                    std::println(stderr, "autosave: {}", got.error());
                }
            }
        }
        std::ifstream file(spell_path);
        std::stringstream text;
        text << file.rdbuf();
        if (auto graph = Runes::parse_graph(text.str())) editor.reset(std::move(*graph));
        else std::println(stderr, "{}: {}", spell_path, graph.error().format());
    }

    /// Правка → перекомпиляция → подмена программы слота 1 (если собралось; иначе остаётся прежняя рабочая версия).
    void refresh() {
        if (editor.revision() == analyzed_revision) return;
        analyzed_revision = editor.revision();
        analysis = ed::analyze(editor.graph(), sim->config().runes, edit_program);
        if (analysis.ok()) sim->programs().add_program(edit_program, *analysis.program);
    }

    void editor_input(const WindowSystem::InputState& in, glm::vec2 vp) {
        const Rect area = editor_area(vp);
        const ed::Vec2 local{cursor.x - area.position.x, cursor.y - area.position.y};
        const bool ctrl = in.down(GLFW_KEY_LEFT_CONTROL) || in.down(GLFW_KEY_RIGHT_CONTROL), shift = in.down(GLFW_KEY_LEFT_SHIFT) || in.down(GLFW_KEY_RIGHT_SHIFT);
        if (std::exchange(needs_frame, false)) controller.camera.frame(ed::bounds_of(editor.graph()), {area.size.x, area.size.y - palette_height - 30.0f});
        const bool inside = local.x >= 0 && local.y >= 0 && local.x <= area.size.x && local.y <= area.size.y;
        const bool in_palette = inside && local.y < palette_height;

        if (in.mouse_pressed(GLFW_MOUSE_BUTTON_LEFT) && in_palette && !locked) {
            const int i = static_cast<int>(local.x / (area.size.x / static_cast<float>(palette.size())));
            if (i >= 0 && i < static_cast<int>(palette.size())) place_from_palette(palette[static_cast<std::size_t>(i)], area);
        } else if (in.mouse_pressed(GLFW_MOUSE_BUTTON_LEFT) && inside && (!locked)) {
            controller.press(ed::Button::Left, local, shift);
        }
        if (in.mouse_pressed(GLFW_MOUSE_BUTTON_RIGHT) && inside) controller.press(ed::Button::Right, local);
        controller.move(local);
        if (in.mouse_released(GLFW_MOUSE_BUTTON_LEFT)) controller.release(ed::Button::Left, local);
        if (in.mouse_released(GLFW_MOUSE_BUTTON_RIGHT)) controller.release(ed::Button::Right, local);

        if (const double scroll = in.scroll().y; scroll != 0.0 && inside) {
            const ed::Pick& hover = controller.hover();
            const ed::GraphNode* node = hover.kind == ed::Pick::Kind::Node ? editor.graph().find(hover.node) : nullptr;
            if (node && node->rune == Rune::Push && !locked) { // колесо над PUSH меняет число: 0,5 за щелчок (Shift — 5)
                editor.set_value(hover.node, node->value + static_cast<std::int32_t>(scroll * (shift ? 5.0 : 0.5) * 65536.0));
            } else {
                controller.wheel(static_cast<float>(scroll), local);
            }
        }
        if (locked) return;
        if (ctrl && in.pressed(GLFW_KEY_Z)) shift ? editor.redo() : editor.undo();
        if (ctrl && in.pressed(GLFW_KEY_Y)) editor.redo();
        if (!ctrl && (in.pressed(GLFW_KEY_DELETE) || in.pressed(GLFW_KEY_X))) editor.erase_selection();
        if (in.pressed(GLFW_KEY_C)) controller.cancel();
        if (in.pressed(GLFW_KEY_L)) tidy();
        if (in.pressed(GLFW_KEY_F)) controller.camera.frame(ed::bounds_of(editor.graph()), {area.size.x, area.size.y});
        if (in.pressed(GLFW_KEY_E) && editor.selection().size() == 1) editor.set_entry(*editor.selection().begin());
        if (in.pressed(GLFW_KEY_F5)) save_graph();
        if (in.pressed(GLFW_KEY_ENTER)) pending_cast = Runes::ManaSource::Personal; // выстрел из редактора: смотреть, что засветится
    }

    void place_from_palette(Rune rune, const Rect& area) {
        const ed::Vec2 centre{area.size.x * 0.5f, area.size.y * 0.5f};
        const float jitter = static_cast<float>(placed++ % 6) * 24.0f;
        const Runes::NodeId id = controller.place(rune, {centre.x + jitter, centre.y + jitter}, rune == Rune::Push ? 2 << 16 : 0);
        if (id != Runes::no_node && editor.graph().entry == Runes::no_node && Runes::is_statement(rune)) editor.set_entry(id);
    }

    /// Автораскладка как одна правка истории: узлы двигаются операциями, а не подменой графа.
    void tidy() {
        ed::Graph target = editor.graph();
        ed::auto_layout(target);
        editor.begin();
        for (const auto& [id, node] : target.nodes()) editor.move_node(id, {node.x, node.y});
        editor.end();
        controller.camera.frame(ed::bounds_of(editor.graph()), {1200.0f, 680.0f});
    }

    void save_graph() {
        std::ofstream(spell_path) << Runes::serialize(editor.graph());
        std::println("граф сохранён: {}", spell_path);
    }

    /// Какие узлы сработали при последнем касте: карта «руна → узел» из компиляции графа.
    void light_up_trace() {
        const Runes::SpellTrace& trace = sim->spells().last_trace();
        if (!trace.valid || trace.program != edit_program) return;
        for (const auto& entry : trace.entries) {
            if (entry.pc < analysis.source.size()) glow[analysis.source[entry.pc]] = 1.0f;
        }
    }

    // ================================================================ мир

    void world_input(const WindowSystem::InputState& in, const Core::ActionMap& act) {
        const float up = act.axis(in, actions.forward, actions.back), side = act.axis(in, actions.right, actions.left);
        glm::vec2 w{side, -up}; // вверх по экрану — к меньшему z
        if (glm::length(w) > 1.0f) w = glm::normalize(w);
        wish = {Fixed::from_double(w.x), Fixed::from_double(w.y)};
        if (act.pressed(in, actions.jump)) pending_jump = true;
        if (act.pressed(in, actions.cast_personal)) pending_cast = Runes::ManaSource::Personal;
        if (act.pressed(in, actions.cast_ambient)) pending_cast = Runes::ManaSource::Ambient;
    }

    [[nodiscard]] glm::dvec2 player_xz(float alpha) const {
        const Character::Position& p = *sim->world().get<Character::Position>(sim->player());
        const double a = static_cast<double>(alpha);
        const auto lerp = [&](std::int64_t from, std::int64_t to) { return (static_cast<double>(from) + static_cast<double>(to - from) * a) / static_cast<double>(Fixed::one_raw); };
        return {lerp(p.previous.x, p.value.x), lerp(p.previous.z, p.value.z)};
    }

    /// Точка мира под курсором (вид сверху) → направление каста: из глаз к поверхности под курсором.
    [[nodiscard]] Math::FVec3 aim_towards(double x, double z) const {
        const auto eye = Character::eye(sim->world(), sim->player()).to_doubles();
        double y = heights->height_at(x, z);
        if (std::isnan(y)) y = eye[1] - 1.6;
        double dx = x - eye[0], dy = y - eye[1], dz = z - eye[2];
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.01) return Math::quantize_direction(0.0f, -1.0f, 0.0f);
        dx /= len, dy /= len, dz /= len;
        return Math::quantize_direction(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
    }

    [[nodiscard]] glm::dvec2 cursor_world(glm::vec2 vp) const {
        const glm::dvec2 c = player_xz(1.0f);
        return {c.x + static_cast<double>(cursor.x - vp.x * 0.5f) * view_meters / static_cast<double>(vp.y), c.y + static_cast<double>(cursor.y - vp.y * 0.5f) * view_meters / static_cast<double>(vp.y)};
    }

    void gather_live() {
        if (wish.x != sent_wish.x || wish.z != sent_wish.z) {
            live.push_back(SpellSim::move_command(wish.x, wish.z));
            sent_wish = wish;
        }
        if (pending_jump) live.push_back(SpellSim::jump_command());
        if (pending_cast) {
            if (!editor_open) last_aim_world = cursor_world(last_viewport);
            live.push_back(SpellSim::cast_command(editor_open ? 0 : slot, *pending_cast, aim_towards(last_aim_world.x, last_aim_world.y)));
        }
        pending_jump = false;
        pending_cast.reset();
    }

    // ================================================================ оверлей

    [[nodiscard]] Image mana_image() const {
        Image image(64, 64, Colors::transparent);
        const double base = sim->mana().config().base.to_double();
        const auto eye = Character::eye(sim->world(), sim->player()).to_doubles();
        const std::int64_t y = std::clamp<std::int64_t>(static_cast<std::int64_t>(eye[1] / 2.0), 0, 31);
        for (int z = 0; z < 64; ++z) {
            for (int x = 0; x < 64; ++x) {
                const double density = sim->mana().at(x, y, z).to_double() / std::max(base, 1.0);
                if (density < 0.98) image.set_pixel(x, z, Color{40, 10, 80, static_cast<std::uint8_t>(std::clamp((1.0 - density) * 240.0, 0.0, 200.0))}); // дыра в поле
                else if (density > 1.02) image.set_pixel(x, z, Color{90, 190, 255, static_cast<std::uint8_t>(std::clamp((density - 1.0) * 200.0, 0.0, 160.0))});
                else image.set_pixel(x, z, Color{80, 130, 255, 40});
            }
        }
        return image;
    }

    void draw_palette(Renderer2D& r, FontHandle font, const Rect& area) const {
        const float w = area.size.x / static_cast<float>(palette.size());
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const glm::vec2 at = area.position + glm::vec2{static_cast<float>(i) * w, 0.0f};
            const Color c = ed::family_color(ed::family_of(palette[i]));
            r.fill_rect({at + 2.0f, {w - 4.0f, palette_height - 4.0f}}, mix(Color{0, 0, 0, 230}, c, locked ? 0.15f : 0.4f), 30);
            r.draw_text(font, std::string(Runes::rune_name(palette[i])), at + glm::vec2{8.0f, 6.0f}, {.size = 15.0f, .layer = 31});
        }
    }

    void draw_hud(Core::App& app, Renderer2D& r, FontHandle font, glm::vec2 vp) {
        last_viewport = vp;
        const Character::ManaPool& pool = *sim->world().get<Character::ManaPool>(sim->player());
        const Runes::SpellTrace& trace = sim->spells().last_trace();
        std::vector<std::string> lines{
            std::format("tick {} | {} | mana field {:+.0f} vs base, {} chunks | {}", sim->tick_number(), locked ? (session->mode() == Replay::Session::Mode::Replay ? "REPLAY" : "REC") : "live",
                        static_cast<double>(sim->mana().excess_raw()) / 65536.0, sim->mana().allocated_chunks(), editor_open ? "Tab: back to world" : "Tab: rune editor"),
        };
        if (trace.valid) {
            lines.push_back(std::format("last spell '{}' [{}]: {} runes, {:.1f} mana, {}", trace.program, trace.source == Runes::ManaSource::Ambient ? "ambient" : "personal", trace.runes_executed,
                                        trace.spent.to_double(), trace.status == Runes::Status::Failed ? std::string(Runes::failure_text(trace.failure)) : "ok"));
        }
        if (!analysis.ok() && analysis.error) lines.push_back("edit slot uses the previous valid graph: " + analysis.error->format());
        overlay::panel(r, font, {10.0f, editor_open ? palette_height + 20.0f : 10.0f}, lines, 15.0f);

        const float bar_width = 360.0f;
        const glm::vec2 base{vp.x * 0.5f - bar_width * 0.5f, vp.y - 70.0f};
        overlay::bar(r, {base, {bar_width, 16.0f}}, static_cast<float>(pool.current.to_double() / pool.max.to_double()), Color::from_rgba(0x4F8CFFFF));
        const auto& names = sim->world().get<Character::Grimoire>(sim->player())->slots;
        for (int i = 0; i < Character::Grimoire::slot_count; ++i) {
            const glm::vec2 at = base + glm::vec2{static_cast<float>(i) * (bar_width / 3.0f), 22.0f};
            r.fill_rect({at, {bar_width / 3.0f - 6.0f, 30.0f}}, i == slot ? Color{90, 70, 200, 220} : Color{0, 0, 0, 150}, 10);
            r.draw_text(font, std::format("{} {}", i + 1, names[static_cast<std::size_t>(i)]), at + glm::vec2{8.0f, 6.0f}, {.size = 15.0f, .layer = 12});
        }
        if (sim->tick_number() < failure_until) r.draw_text(font, last_failure, {vp.x * 0.5f - 130.0f, vp.y * 0.5f + 60.0f}, {.size = 18.0f, .color = Color::from_rgba(0xFF7070FF), .layer = 12, .shadow = Colors::black});
        (void)app;
    }

    // ================================================================ автоигра

    struct Check {
        std::string what;
        bool ok;
    };
    void expect(std::string what, bool ok) {
        checks.push_back({std::move(what), ok});
        if (!ok) ++g_failed_checks;
    }

    /// Сценарий без ввода: граф строится жестами и правками, затем выстрелы, undo/redo, восстановление автосохранения после порчи.
    void gather_autoplay(std::uint32_t t) {
        const Math::FVec3 down_east = Math::normalize({Fixed::from_ratio(1, 2), Fixed::from_ratio(-1, 2), Fixed{}});
        // В записи редактор заблокирован (граф — настройка прогона, не команда): сценарий только ходит и стреляет графом из файла.
        const bool edits = !locked;
        switch (t) {
        case 2: if (edits) autoplay_build_graph(); break;
        case 20: live.push_back(SpellSim::move_command(Fixed::from_int(1), Fixed{})); break;
        case 70: // выстрел из слота 1 — собранным графом
            hash_before_cast = sim->hashes().at("terrain");
            mana_before_cast = sim->world().get<Character::ManaPool>(sim->player())->current;
            live.push_back(SpellSim::cast_command(0, Runes::ManaSource::Personal, down_east));
            break;
        case 75: if (edits) autoplay_after_cast(); break;
        case 90: if (edits) autoplay_edit_and_undo(); break;
        case 100: live.push_back(SpellSim::cast_command(0, Runes::ManaSource::Ambient, down_east)); break;
        case 110: if (edits) autoplay_autosave(); break;
        default: break;
        }
    }

    void autoplay_build_graph() {
        controller.camera.zoom = 1.0f;
        editor.select_all(); // стартовый граф из файла убираем: в сценарии строится свой
        editor.erase_selection();
        // Узлы — из «палитры», рёбра — жестами мыши от порта к порту (как рукой).
        const Runes::NodeId target = controller.place(Rune::Target, {120, 140});
        const Runes::NodeId radius = controller.place(Rune::Push, {120, 260}, 3 << 16);
        const Runes::NodeId carve = controller.place(Rune::Carve, {360, 180});
        const Runes::NodeId halt = controller.place(Rune::Halt, {560, 180});
        const auto drag = [&](const ed::PortRef& from, const ed::PortRef& to) {
            // Цель — порт; для цепочки управления (у HALT входов нет) — тело узла.
            const auto target_point = [&](const ed::PortRef& p) {
                if (const auto at = ed::port_position(editor.graph(), p)) return *at;
                const ed::GraphNode& n = *editor.graph().find(p.node);
                return ed::Vec2{n.x, n.y};
            };
            const ed::Vec2 a = controller.camera.to_screen(*ed::port_position(editor.graph(), from)), b = controller.camera.to_screen(target_point(to));
            controller.press(ed::Button::Left, a);
            controller.move(b);
            controller.release(ed::Button::Left, b);
        };
        drag({target, ed::PortKind::Output, 0}, {carve, ed::PortKind::Input, 0});
        editor.set_entry(carve);
        drag({carve, ed::PortKind::Next, 0}, {halt, ed::PortKind::Input, 0});
        refresh();
        expect("граф без радиуса не собирается, слот 1 держит прежнюю программу", !analysis.ok() && sim->programs().find(edit_program) != nullptr);
        drag({radius, ed::PortKind::Output, 0}, {carve, ed::PortKind::Input, 1});
        refresh();
        expect("после подключения радиуса граф собран", analysis.ok());
        expect("цена прохода оценена (CARVE, радиус 3)", analysis.ok() && analysis.cost.effects == 1 && analysis.cost.exact && analysis.cost.per_pass.raw() > 0);
        built_graph = editor.graph();
        radius_node = radius;
        carve_node = carve;
    }

    void autoplay_after_cast() {
        const Math::Mana pool = sim->world().get<Character::ManaPool>(sim->player())->current;
        expect("выстрел графом изменил ландшафт", sim->hashes().at("terrain") != hash_before_cast);
        expect("выстрел потратил личную ману", pool < mana_before_cast);
        expect("трасса подсветила узлы графа", !glow.empty() && glow.contains(carve_node));
        std::println("autoplay: после выстрела светятся {} узлов, потрачено {:.1f} маны", glow.size(), (mana_before_cast - pool).to_double());
    }

    void autoplay_edit_and_undo() {
        const Math::Mana before = analysis.cost.per_pass;
        editor.set_value(radius_node, 2 << 16); // радиус 3 → 2: цена эффекта падает в (3/2)³ раза
        refresh();
        expect("правка радиуса меняет цену", analysis.ok() && analysis.cost.per_pass < before);
        const ed::Graph edited = editor.graph();
        expect("undo возвращает прежний граф", editor.undo() && editor.graph() == built_graph);
        refresh();
        expect("redo возвращает правку", editor.redo() && editor.graph() == edited);
        refresh();
    }

    void autoplay_autosave() {
        if (!autosave || !storage) return expect("автосохранение запущено", false);
        autosave->flush();
        // Порча одного блока носителя: чётность Рида—Соломона должна вернуть граф целиком.
        std::byte junk[8];
        for (std::byte& b : junk) b = std::byte{0xAB};
        storage->write(EventLog::block_offset(EventLog::Config{}, 0, 0) + 24, junk);
        const auto recovered = ed::Autosave::recover(*storage);
        expect("автосохранение восстанавливается после порчи блока", recovered.has_value() && recovered->graph == editor.graph());
        expect("порча блока исправлена по чётности", recovered.has_value() && recovered->report.blocks_repaired >= 1);
        // Восстановленный граф собирается в тот же байт-код, что и рабочий.
        if (recovered) expect("восстановленный граф даёт тот же байт-код", ed::analyze(recovered->graph).program->code.size() == analysis.program->code.size());
    }

    void finish_autoplay() {
        expect("весь сценарий дожил до конца (≥ 120 тиков)", sim->tick_number() >= 120);
        for (const Check& c : checks) std::println("  [{}] {}", c.ok ? "ok" : "ОШИБКА", c.what);
        if (g_failed_checks == 0) std::println("RuneCell2: OK");
        else std::println("RuneCell2: ОШИБКА ({} проверок)", g_failed_checks);
    }

    // ---- состояние
    std::unique_ptr<SpellSim::Simulation> sim;
    Actions actions;
    Replay::CommandRegistry command_registry;
    std::optional<Replay::Session> session;
    Replay::FlightRecorder recorder{512};
    std::optional<Replay::Driver<SpellSim::Simulation>> driver;
    es::EventReader<Runes::SpellFailedEvent> failures;

    ed::Editor editor;
    ed::Controller controller{editor};
    ed::Analysis analysis;
    std::uint64_t analyzed_revision = ~0ull;
    std::optional<ed::View> editor_view;
    std::unique_ptr<EventLog::FileStorage> storage;
    std::optional<ed::Autosave> autosave;
    std::uint64_t flushed_ops = 0;
    std::map<Runes::NodeId, float> glow;
    std::string spell_path;

    std::optional<WorldRender::HeightMap> heights;
    std::optional<WorldRender::TerrainHeights> height_source;
    TextureHandle height_tex, mana_tex, disc, ring;
    bool heights_dirty = false;
    std::uint32_t mana_drawn_tick = ~0u;

    struct Wish {
        Fixed x{}, z{};
    } wish, sent_wish;
    std::vector<Replay::Command> live;
    std::optional<Runes::ManaSource> pending_cast;
    bool pending_jump = false;
    int slot = 0;
    bool editor_open = false, locked = false, autoplay = false, needs_frame = true;
    glm::vec2 cursor{0.0f}, last_viewport{1280.0f, 720.0f};
    glm::dvec2 last_aim_world{64.0, 64.0};
    int placed = 0;
    std::string last_failure;
    std::uint32_t failure_until = 0;
    std::uint64_t frames = 0;

    // автоигра
    std::vector<Check> checks;
    ed::Graph built_graph;
    Runes::NodeId radius_node = Runes::no_node, carve_node = Runes::no_node;
    std::uint64_t hash_before_cast = 0;
    Math::Mana mana_before_cast{};
};

} // namespace

int main(int argc, char** argv) {
    const int code = Core::run<RuneCell2>({.title = "RuneCell2", .ticks_per_second = 60.0, .pause_key = GLFW_KEY_P, .camera_controls = false, .clear_rgba = 0x0A0C16FF}, argc, argv);
    return code != 0 ? code : (g_failed_checks != 0 ? 1 : 0);
}
