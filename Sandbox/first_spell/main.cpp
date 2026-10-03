/**
 * @file main.cpp
 * @brief FirstSpell — пре-альфа «Первое заклинание»: персонаж на гладком SDF-ландшафте запускает заклинание,
 * туман маны вокруг редеет, а земля перед ним поднимается или вырезается.
 *
 * Вся игровая логика живёт в модулях движка, здесь только склейка:
 * - **SpellSim** — мир и порядок тика (команды → заклинания → ландшафт → поле маны → движение → события);
 * - **Replay** — `--record файл` / `--replay файл`, журнал последних тиков при FLUX_ASSERT;
 * - **WorldRender** — камеры, ландшафт, туман, линии, помощники оверлея;
 * - **Core** — окно, тик 60 Гц, ввод; **Runes**, **Terrain**, **ManaField**, **Character**, **Math** — через SpellSim.
 *
 * Игра делает три вещи: превращает ввод в команды (float → Fixed только здесь, один раз), ведёт камеру и рисует.
 *
 * Управление: WASD — ходьба, Space — прыжок, мышь — камера, 1–3 — слот гримуара, ЛКМ — каст из личного запаса,
 * ПКМ — каст из окружающей маны, C — свободная камера (WASD + Q/E + Shift), Tab — отпустить мышь,
 * F2 — каркас, F3 — рамки чанков, F4 — срез поля маны, F5 — перечитать заклинания, F6 — трасса заклинания.
 *
 * Клавиши — это действия (`Actions`): `--print-bindings` печатает привязки в формате файла, `--bindings файл` загружает свои.
 * Аргументы: `--seed N`, `--record файл`, `--replay файл`, `--spells каталог`, `--autoplay` (сценарий без ввода),
 * `--show slice,trace,chunks,wire` (отладочные слои сразу).
 * Общие (`--ticks`, `--threads`, `--screenshot`, `--backend`) — см. Core::App.
 */

#include <Core/Core.hpp>
#include <SpellSim/SpellSim.hpp>
#include <WorldRender/Adapters/Terrain.hpp>
#include <WorldRender/WorldRender.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <sstream>

#ifndef FIRST_SPELL_SPELLS_DIR
#define FIRST_SPELL_SPELLS_DIR "spells"
#endif

namespace es = EventSystem;
using namespace RendererSystem;
using Math::Fixed;
using Math::FVec3;
namespace overlay = WorldRender::overlay;
using WorldRender::TerrainView;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

// Граница с float (glm ↔ Math): вся конверсия — в Math, здесь только переходники на типы glm.
FVec3 to_fvec(glm::vec3 v) { return Math::quantize_direction(v.x, v.y, v.z); }
glm::dvec3 to_meters(Math::WorldPos p) { const auto m = p.to_doubles(); return {m[0], m[1], m[2]}; }
Math::WorldPos to_world(const glm::dvec3& p) { return Math::WorldPos::from_doubles(p.x, p.y, p.z); }

/// Действия игры и их привязки по умолчанию. Игрок переопределяет их файлом (`--bindings файл`, формат — `ActionMap::serialize`).
struct Actions {
    Core::ActionMap map;
    Core::ActionId forward, back, left, right, jump, cast_personal, cast_ambient, free_camera, fly_up, fly_down, fly_fast;
    Core::ActionId toggle_capture, toggle_wireframe, toggle_chunks, toggle_slice, toggle_trace, reload_spells;
    std::array<Core::ActionId, Character::Grimoire::slot_count> slot;

    Actions() {
        using Core::Binding;
        forward = map.declare("move_forward", "Вперёд");
        back = map.declare("move_back", "Назад");
        left = map.declare("move_left", "Влево");
        right = map.declare("move_right", "Вправо");
        jump = map.declare("jump", "Прыжок");
        cast_personal = map.declare("cast_personal", "Каст из личного запаса маны");
        cast_ambient = map.declare("cast_ambient", "Каст из окружающей маны");
        free_camera = map.declare("camera_free", "Свободная камера");
        fly_up = map.declare("fly_up", "Свободная камера: вверх");
        fly_down = map.declare("fly_down", "Свободная камера: вниз");
        fly_fast = map.declare("fly_fast", "Свободная камера: быстро");
        toggle_capture = map.declare("toggle_mouse_capture", "Отпустить / захватить мышь");
        toggle_wireframe = map.declare("toggle_wireframe", "Каркасный режим ландшафта");
        toggle_chunks = map.declare("toggle_chunk_boxes", "Рамки чанков");
        toggle_slice = map.declare("toggle_mana_slice", "Срез поля маны");
        toggle_trace = map.declare("toggle_spell_trace", "Трасса последнего заклинания");
        reload_spells = map.declare("reload_spells", "Перечитать файлы заклинаний");
        for (std::size_t i = 0; i < slot.size(); ++i) slot[i] = map.declare("slot_" + std::to_string(i + 1), "Слот гримуара " + std::to_string(i + 1));

        map.bind(forward, Binding::key(GLFW_KEY_W)).bind(forward, Binding::key(GLFW_KEY_UP));
        map.bind(back, Binding::key(GLFW_KEY_S)).bind(back, Binding::key(GLFW_KEY_DOWN));
        map.bind(left, Binding::key(GLFW_KEY_A)).bind(left, Binding::key(GLFW_KEY_LEFT));
        map.bind(right, Binding::key(GLFW_KEY_D)).bind(right, Binding::key(GLFW_KEY_RIGHT));
        map.bind(jump, Binding::key(GLFW_KEY_SPACE));
        map.bind(cast_personal, Binding::mouse(GLFW_MOUSE_BUTTON_LEFT));
        map.bind(cast_ambient, Binding::mouse(GLFW_MOUSE_BUTTON_RIGHT));
        map.bind(free_camera, Binding::key(GLFW_KEY_C));
        map.bind(fly_up, Binding::key(GLFW_KEY_E)).bind(fly_down, Binding::key(GLFW_KEY_Q));
        map.bind(fly_fast, Binding::key(GLFW_KEY_LEFT_SHIFT));
        map.bind(toggle_capture, Binding::key(GLFW_KEY_TAB));
        map.bind(toggle_wireframe, Binding::key(GLFW_KEY_F2)).bind(toggle_chunks, Binding::key(GLFW_KEY_F3));
        map.bind(toggle_slice, Binding::key(GLFW_KEY_F4)).bind(reload_spells, Binding::key(GLFW_KEY_F5));
        map.bind(toggle_trace, Binding::key(GLFW_KEY_F6));
        for (std::size_t i = 0; i < slot.size(); ++i) map.bind(slot[i], Binding::key(GLFW_KEY_1 + static_cast<int>(i)));
    }
};

class FirstSpell final : public Core::Game {
public:
    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        std::string spells_dir = FIRST_SPELL_SPELLS_DIR;
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--spells") spells_dir = args[i + 1];
        }
        autoplay = std::ranges::find(args, "--autoplay") != args.end();
        for (std::size_t i = 0; i + 1 < args.size(); ++i) { // --bindings файл: привязки игрока (формат — Actions::map.serialize())
            if (args[i] != "--bindings") continue;
            std::ifstream file(args[i + 1]);
            std::stringstream text;
            text << file.rdbuf();
            if (auto ok = actions.map.apply(text.str()); !ok) throw std::runtime_error(args[i + 1] + ": " + ok.error());
        }
        if (std::ranges::find(args, "--print-bindings") != args.end()) std::print("{}", actions.map.serialize());
        for (std::size_t i = 0; i + 1 < args.size(); ++i) { // --show slice,trace,chunks,wire: включить отладочные слои сразу
            if (args[i] != "--show") continue;
            show_slice = args[i + 1].contains("slice"), show_trace = args[i + 1].contains("trace"), show_chunks = args[i + 1].contains("chunks");
            wire_on_start = args[i + 1].contains("wire");
        }

        SpellSim::register_commands(command_registry);
        auto parsed = Replay::Session::from_args(args, 1, &command_registry);
        if (!parsed) throw std::runtime_error(parsed.error());
        session.emplace(std::move(*parsed));

        sim = std::make_unique<SpellSim::Simulation>(SpellSim::Config{.seed = session->seed()});
        sim->declare(app.bus());
        const es::ModuleId me = app.bus().declare_module("FirstSpell").consumes<Runes::SpellFailedEvent>().consumes<SpellSim::TerrainEditedEvent>();
        failures = app.bus().reader<Runes::SpellFailedEvent>(me);
        edits = app.bus().reader<SpellSim::TerrainEditedEvent>(me);
        reload_spells(spells_dir);
        spells_path = spells_dir;
        recorder.install_assert_dump("flight_recorder.txt");
        driver.emplace(*sim, *session, &recorder);

        surface.emplace(sim->terrain());           // источники данных для рендера: рендер не знает ни Terrain, ни ManaField
        fog_source.emplace(sim->mana(), sim->terrain());
        terrain_view.emplace(app.device(), *surface);
        fog_view.emplace(app.device());
        line_renderer.emplace(app.device());

        // Стартовый мир целиком, сразу (дальше — по 4 чанка за кадр).
        terrain_view->enqueue(WorldRender::TerrainSurface::indices(sim->take_dirty_chunks()));
        const glm::dvec3 start = to_meters(Character::eye(sim->world(), sim->player()));
        terrain_view->update(app.jobs(), start, 0);
        rig.yaw = 0.0f;
        terrain_view->set_wireframe(wire_on_start);
        capture(app, !autoplay && session->mode() != Replay::Session::Mode::Replay);
        std::println("FirstSpell: seed {} | {} | {} spells loaded | startup mesh {:.1f} ms ({} triangles)", session->seed(),
                     session->mode() == Replay::Session::Mode::Replay ? "REPLAY" : session->mode() == Replay::Session::Mode::Record ? "RECORD" : "live",
                     sim->programs().names().size(), terrain_view->stats().build_ms, terrain_view->stats().triangles);
    }

    // ---------------------------------------------------------------- кадр: ввод, камера, сетки на GPU

    void frame(Core::App& app, float seconds) override {
        frame_ms.add(static_cast<double>(seconds) * 1000.0);
        worst_frame_ms = std::max(worst_frame_ms, static_cast<double>(seconds) * 1000.0);
        ++frames;
        WindowSystem::Window& window = app.window();
        const auto& in = window.input();

        // Игра спрашивает про действия, а не про клавиши: привязки живут в `Actions` и в файле игрока.
        const Core::ActionMap& act = actions.map;
        if (act.pressed(in, actions.toggle_capture)) capture(app, !captured);
        if (act.pressed(in, actions.free_camera)) rig.toggle();
        if (act.pressed(in, actions.toggle_wireframe)) terrain_view->set_wireframe(!terrain_view->wireframe());
        if (act.pressed(in, actions.toggle_chunks)) show_chunks = !show_chunks;
        if (act.pressed(in, actions.toggle_slice)) show_slice = !show_slice;
        if (act.pressed(in, actions.toggle_trace)) show_trace = !show_trace;
        if (act.pressed(in, actions.reload_spells)) reload_spells(spells_path);
        for (int i = 0; i < Character::Grimoire::slot_count; ++i) {
            if (act.pressed(in, actions.slot[static_cast<std::size_t>(i)])) slot = i;
        }
        if (captured) {
            const WindowSystem::Vec2d d = in.cursor_delta();
            rig.look(static_cast<float>(d.x), static_cast<float>(d.y));
        }

        const float forward_axis = act.axis(in, actions.forward, actions.back), right_axis = act.axis(in, actions.right, actions.left);
        if (rig.is_free()) {
            rig.free_speed = act.down(in, actions.fly_fast) ? 60.0f : 18.0f;
            rig.fly(forward_axis, right_axis, act.axis(in, actions.fly_up, actions.fly_down), seconds);
            wish = {};
        } else {
            // Ходьба относительно камеры: ввод → направление в плоскости, квантованное в Fixed один раз.
            const glm::vec3 f = glm::normalize(glm::vec3{std::cos(rig.yaw), 0.0f, std::sin(rig.yaw)});
            const glm::vec3 r{-f.z, 0.0f, f.x};
            glm::vec3 w = f * forward_axis + r * right_axis;
            if (glm::length(w) > 1.0f) w = glm::normalize(w);
            wish = {Fixed::from_double(w.x), Fixed::from_double(w.z)};
            if (act.pressed(in, actions.jump)) pending_jump = true;
            if (captured && act.pressed(in, actions.cast_personal)) pending_cast = Runes::ManaSource::Personal;
            if (captured && act.pressed(in, actions.cast_ambient)) pending_cast = Runes::ManaSource::Ambient;
        }

        const glm::dvec3 eye = current_eye(app.tick_alpha());
        view = rig.view(eye, sim->terrain(), app.camera().viewport);
        aim_hit = sim->terrain().raycast(to_world(view.eye), to_fvec(rig.forward()), Fixed::from_int(120));

        // Новые сетки: не больше четырёх чанков за кадр, ближние к камере — первыми.
        const auto t0 = Clock::now();
        terrain_view->update(app.jobs(), view.eye, 4);
        mesh_ms.add(ms_since(t0));
    }

    // ------------------------------------------------------------------------ тик: команды → симуляция

    void tick(Core::App& app) override {
        const std::uint32_t t = sim->tick_number();
        live.clear();
        if (!driver->replaying()) gather_commands(t);
        if (!driver->step(live)) { // повтор дошёл до конца записи
            app.window().request_close();
            return;
        }
        terrain_view->enqueue(WorldRender::TerrainSurface::indices(sim->take_dirty_chunks())); // фаза 6: изменённые чанки — в очередь на перестройку
        tick_ms.add(sim->tick_ms());
        worst_tick_ms = std::max(worst_tick_ms, sim->tick_ms());

        for (const Runes::SpellFailedEvent& f : failures.events()) {
            last_failure = std::format("spell failed: {} (rune {})", Runes::failure_text(static_cast<Runes::Failure>(f.reason)), f.pc);
            failure_until = t + 60 * 4;
        }
        for (const SpellSim::TerrainEditedEvent& e : edits.events()) last_edit_chunks = e.chunks;
    }

    // ------------------------------------------------------------------------------------ рисование

    void render_3d(Core::App& app, Renderer3D& r3d) override {
        const auto t0 = Clock::now();
        terrain_view->draw(view, light);

        // Персонаж: капсула из трёх сфер по высоте коллайдера и маркер цели луча.
        const glm::dvec3 feet = to_meters(interpolated_feet(app.tick_alpha()));
        Environment env;
        env.sun.direction = light.direction;
        env.ambient = Color{90, 95, 115, 255};
        r3d.begin(view.camera, env);
        const auto place = [&](glm::dvec3 world, glm::vec3 scale) { return glm::scale(glm::translate(glm::mat4{1.0f}, view.relative(world)), scale); };
        const Color robe = Color::from_rgba(0x6A5ACDFF);
        r3d.draw_shape(Renderer3D::Shape::Sphere, place(feet + glm::dvec3{0, 0.4, 0}, glm::vec3{0.8f}), {.color = robe});
        r3d.draw_shape(Renderer3D::Shape::Cylinder, place(feet + glm::dvec3{0, 0.9, 0}, glm::vec3{0.8f, 1.0f, 0.8f}), {.color = robe});
        r3d.draw_shape(Renderer3D::Shape::Sphere, place(feet + glm::dvec3{0, 1.4, 0}, glm::vec3{0.8f}), {.color = Color::from_rgba(0xF2C9A0FF)});
        if (aim_hit) {
            r3d.draw_shape(Renderer3D::Shape::Sphere, place(to_meters(aim_hit->position), glm::vec3{0.25f}),
                           {.color = Color::from_rgba(0xFFE070FF), .emissive = {1.0f, 0.85f, 0.3f}, .lit = false});
        }
        r3d.end();

        fog_view->draw(*fog_source, view);

        if (show_chunks) TerrainView::add_chunk_boxes(lines, surface->shape(), Color{255, 255, 255, 70});
        line_renderer->flush(lines, view);
        draw_ms.add(ms_since(t0));
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        const glm::vec2 vp = app.camera().viewport;
        const FontHandle font = app.ui_font();
        std::string phases = std::format("tick {:.2f} ms:", sim->tick_ms()); // фазы берутся из расписания: новая фаза появится в оверлее сама
        for (const Phases::PhaseTime& t : sim->schedule().times()) phases += std::format(" {} {:.2f}", t.name, t.milliseconds);
        const auto& stats = terrain_view->stats();
        const Runes::SpellTrace& trace = sim->spells().last_trace();
        const Character::ManaPool& pool = *sim->world().get<Character::ManaPool>(sim->player());

        std::vector<std::string> lines_text{
            std::format("{:.0f} FPS | frame {:.1f} ms | render {:.1f} ms | mesh {:.2f} ms", 1000.0 / std::max(frame_ms.value, 0.001), frame_ms.value, draw_ms.value, mesh_ms.value),
            phases,
            std::format("chunks {}/{} drawn | mesh queue {} | {:.0f}k tris | {:.1f} MiB gpu | last edit {} chunks", stats.drawn, stats.drawn + stats.culled,
                        stats.queued, static_cast<double>(stats.triangles) / 1000.0, static_cast<double>(stats.gpu_bytes) / (1024.0 * 1024.0), last_edit_chunks),
            std::format("mana field: {:+.0f} vs base, {} chunks | tick {} | {} | camera: {}", static_cast<double>(sim->mana().excess_raw()) / 65536.0,
                        sim->mana().allocated_chunks(), sim->tick_number(),
                        session->mode() == Replay::Session::Mode::Replay ? "REPLAY" : session->mode() == Replay::Session::Mode::Record ? "REC" : "live",
                        rig.is_free() ? "free" : "follow"),
        };
        if (trace.valid) {
            lines_text.push_back(std::format("last spell '{}' [{}]: {} runes, {:.1f} mana, {}{}", trace.program,
                                             trace.source == Runes::ManaSource::Ambient ? "ambient" : "personal", trace.runes_executed, trace.spent.to_double(),
                                             trace.status == Runes::Status::Running ? "running" : trace.status == Runes::Status::Halted ? "done" : "FAILED ",
                                             trace.status == Runes::Status::Failed ? std::string(Runes::failure_text(trace.failure)) : std::string{}));
        }
        overlay::panel(r, font, {10.0f, 10.0f}, lines_text, 16.0f);

        // Личная мана и слоты гримуара.
        const float bar_width = 360.0f;
        const glm::vec2 base{vp.x * 0.5f - bar_width * 0.5f, vp.y - 86.0f};
        overlay::bar(r, {base, {bar_width, 18.0f}}, static_cast<float>(pool.current.to_double() / pool.max.to_double()), Color::from_rgba(0x4F8CFFFF));
        r.draw_text(font, std::format("{:.0f} / {:.0f}", pool.current.to_double(), pool.max.to_double()), base + glm::vec2{bar_width * 0.5f - 40.0f, 0.0f},
                    {.size = 14.0f, .layer = 13});
        const auto& names = sim->world().get<Character::Grimoire>(sim->player())->slots;
        for (int i = 0; i < Character::Grimoire::slot_count; ++i) {
            const glm::vec2 at = base + glm::vec2{static_cast<float>(i) * (bar_width / 3.0f), 26.0f};
            r.fill_rect({at, {bar_width / 3.0f - 6.0f, 34.0f}}, i == slot ? Color{90, 70, 200, 220} : Color{0, 0, 0, 150}, 10);
            r.draw_text(font, std::format("{} {}", i + 1, names[static_cast<std::size_t>(i)]), at + glm::vec2{8.0f, 7.0f}, {.size = 16.0f, .layer = 12});
        }
        overlay::crosshair(r, vp, aim_hit ? Colors::white : Color{255, 255, 255, 110});
        if (sim->tick_number() < failure_until) {
            r.draw_text(font, last_failure, {vp.x * 0.5f - 130.0f, vp.y * 0.5f + 40.0f}, {.size = 18.0f, .color = Color::from_rgba(0xFF7070FF), .layer = 12, .shadow = Colors::black});
        }
        if (show_trace && trace.valid) {
            std::vector<std::string> trace_lines{std::format("trace '{}' ({} runes, {:.1f} mana)", trace.program, trace.runes_executed, trace.spent.to_double())};
            const std::size_t first = trace.entries.size() > 14 ? trace.entries.size() - 14 : 0;
            for (std::size_t i = first; i < trace.entries.size(); ++i) {
                const auto& e = trace.entries[i];
                trace_lines.push_back(std::format("{:>3} {:<7} {:.2f}", e.pc, Runes::rune_name(e.rune), e.cost.to_double()));
            }
            overlay::panel(r, font, {vp.x - 260.0f, vp.y - 340.0f}, trace_lines, 15.0f);
        }
        if (show_slice) { // горизонтальный срез поля на высоте персонажа: тепловая карта 64×64
            const Terrain::Layout& layout = sim->terrain().layout(); // ячейки поля над ландшафтом
            const int slice_x = layout.samples_x() / 4, slice_y = layout.samples_y() / 4, slice_z = layout.samples_z() / 4;
            const int y = std::clamp(static_cast<int>(view.eye.y / 2.0), 0, slice_y - 1);
            const float base_density = static_cast<float>(sim->mana().config().base.to_double());
            const Rect box{{vp.x - 290.0f, 14.0f}, {272.0f, 272.0f}};
            overlay::heatmap(r, box, slice_x, slice_z,
                             [&](int x, int z) { return static_cast<float>(sim->mana().at(x, y, z).to_double()) / (base_density * 1.6f); });
            const glm::dvec3 p = to_meters(Character::eye(sim->world(), sim->player()));
            r.fill_rect({{box.position.x + static_cast<float>(p.x / 2.0) * 4.25f - 2.0f, box.position.y + static_cast<float>(p.z / 2.0) * 4.25f - 2.0f}, {5.0f, 5.0f}}, Colors::white, 13);
            r.draw_text(font, std::format("mana slice y={} (F4)", y), box.position + glm::vec2{0.0f, 276.0f}, {.size = 14.0f, .layer = 13});
        }
    }

    [[nodiscard]] std::string status() const override {
        return std::format("tick {} | {:.0f} FPS | mana {:.0f}", sim ? sim->tick_number() : 0, 1000.0 / std::max(frame_ms.value, 0.001),
                           sim ? sim->world().get<Character::ManaPool>(sim->player())->current.to_double() : 0.0);
    }

    void shutdown(Core::App& app) override {
        const std::uint32_t ticks = sim->tick_number();
        const Replay::StateHashes h = sim->hashes();
        std::println("\n===== FirstSpell: {} ticks, {} frames =====", ticks, frames);
        std::println("hash terrain {:016x} mana {:016x} ecs {:016x}", h.value[0], h.value[1], h.value[2]);
        std::println("casts {} (empty slot {}), terrain edits {}, jobs threads {}", sim->casts(), sim->failed_casts(), sim->edits_applied(), app.jobs().threads());
        std::println("tick avg {:.3f} ms worst {:.3f} ms (budget 4) | frame avg {:.2f} ms worst {:.2f} ms (budget 16.6) | mesh/frame {:.3f} ms", tick_ms.value,
                     worst_tick_ms, frame_ms.value, worst_frame_ms, mesh_ms.value);
        if (auto verdict = driver->finish()) {
            if (const std::string text = Replay::describe(*verdict, session->mode(), ticks, session->recording().command_count()); !text.empty()) std::println("{}", text);
        } else {
            std::println(stderr, "replay: {}", verdict.error());
        }
        recorder.uninstall_assert_dump();
        terrain_view.reset();
        fog_view.reset();
        fog_source.reset();
        surface.reset();
        line_renderer.reset();
    }

private:
    void reload_spells(const std::string& dir) {
        const auto report = sim->reload_spells(dir);
        std::println("spells: {} loaded from {}", report.loaded, dir);
        for (const std::string& e : report.errors) std::println(stderr, "spells: {}", e);
        if (!report.errors.empty()) {
            last_failure = report.errors.front();
            failure_until = sim->tick_number() + 60 * 6;
        }
    }

    void capture(Core::App& app, bool on) {
        captured = on;
        app.window().set_cursor_mode(on ? WindowSystem::CursorMode::Captured : WindowSystem::CursorMode::Normal);
    }

    [[nodiscard]] glm::dvec3 current_eye(float alpha) const {
        const glm::dvec3 feet = to_meters(interpolated_feet(alpha));
        return feet + glm::dvec3{0.0, 1.6, 0.0};
    }
    [[nodiscard]] Math::WorldPos interpolated_feet(float alpha) const {
        const Character::Position& p = *sim->world().get<Character::Position>(sim->player());
        const double a = static_cast<double>(alpha);
        return {p.previous.x + std::llround(static_cast<double>(p.value.x - p.previous.x) * a), p.previous.y + std::llround(static_cast<double>(p.value.y - p.previous.y) * a),
                p.previous.z + std::llround(static_cast<double>(p.value.z - p.previous.z) * a)};
    }

    /// Направление каста: из глаз персонажа в точку, куда смотрит камера (параллакс от третьего лица).
    [[nodiscard]] FVec3 aim_direction() const {
        glm::dvec3 target = view.eye + glm::dvec3(rig.forward()) * 60.0;
        if (aim_hit) target = to_meters(aim_hit->position);
        const glm::dvec3 from = to_meters(Character::eye(sim->world(), sim->player()));
        glm::dvec3 d = target - from;
        if (glm::length(d) < 0.01) d = glm::dvec3(rig.forward());
        return to_fvec(glm::vec3(glm::normalize(d)));
    }

    /// Ввод (или сценарий) → команды этого тика. Move шлётся при изменении, Jump и Cast — по нажатию.
    void gather_commands(std::uint32_t t) {
        if (autoplay) return gather_autoplay(t);
        if (wish.x != sent_wish.x || wish.z != sent_wish.z) {
            live.push_back(SpellSim::move_command(wish.x, wish.z));
            sent_wish = wish;
        }
        if (pending_jump) live.push_back(SpellSim::jump_command());
        if (pending_cast) live.push_back(SpellSim::cast_command(slot, *pending_cast, aim_direction()));
        pending_jump = false;
        pending_cast.reset();
    }

    /// Сценарий для проверок без ввода: ходьба, прыжок, каст из личного запаса и из окружения, «убегающее» заклинание.
    void gather_autoplay(std::uint32_t t) {
        const FVec3 down_forward = Math::normalize({Fixed::from_ratio(1, 2), Fixed::from_ratio(-1, 2), Fixed{}});
        switch (t % 600) {
        case 10: live.push_back(SpellSim::move_command(Fixed::from_int(1), Fixed{})); break;
        case 60: live.push_back(SpellSim::cast_command(0, Runes::ManaSource::Personal, down_forward)); break;
        case 120: live.push_back(SpellSim::jump_command()); break;
        case 150: live.push_back(SpellSim::cast_command(1, Runes::ManaSource::Ambient, down_forward)); break;
        case 220: live.push_back(SpellSim::move_command(Fixed{}, Fixed::from_int(1))); break;
        case 260: live.push_back(SpellSim::cast_command(0, Runes::ManaSource::Ambient, down_forward)); break;
        case 340: live.push_back(SpellSim::cast_command(2, Runes::ManaSource::Personal, down_forward)); break;
        case 460: live.push_back(SpellSim::move_command(Fixed::from_ratio(-7, 10), Fixed::from_ratio(7, 10))); break;
        case 520: live.push_back(SpellSim::jump_command()); break;
        default: break;
        }
    }

    std::unique_ptr<SpellSim::Simulation> sim;
    Actions actions;
    Replay::CommandRegistry command_registry; ///< Схемы команд: попадают в файл записи.
    std::optional<Replay::Session> session;
    Replay::FlightRecorder recorder{512};
    std::optional<Replay::Driver<SpellSim::Simulation>> driver;
    std::optional<WorldRender::TerrainSurface> surface;   // источник поверхности для рендера (адаптер Terrain)
    std::optional<WorldRender::ManaFogSource> fog_source; // источник тумана (адаптер ManaField)
    std::optional<WorldRender::TerrainView> terrain_view;
    std::optional<WorldRender::FogView> fog_view;
    std::optional<WorldRender::LineRenderer> line_renderer;
    WorldRender::DebugDraw lines;
    WorldRender::CameraRig rig;
    WorldRender::View view;
    WorldRender::Light light;
    es::EventReader<Runes::SpellFailedEvent> failures;
    es::EventReader<SpellSim::TerrainEditedEvent> edits;

    struct Wish {
        Fixed x{}, z{};
    } wish, sent_wish;
    std::vector<Replay::Command> live;
    std::optional<Terrain::RayHit> aim_hit;
    std::optional<Runes::ManaSource> pending_cast;
    bool pending_jump = false;
    int slot = 0;
    bool wire_on_start = false;
    bool captured = false, autoplay = false, show_chunks = false, show_slice = false, show_trace = false;
    std::string spells_path;
    std::string last_failure;
    std::uint32_t failure_until = 0;
    std::int32_t last_edit_chunks = 0;
    WorldRender::Smoothed frame_ms, tick_ms, mesh_ms, draw_ms;
    double worst_frame_ms = 0.0, worst_tick_ms = 0.0;
    std::uint64_t frames = 0;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<FirstSpell>({.title = "FirstSpell", .ticks_per_second = 60.0, .pause_key = GLFW_KEY_P, .camera_controls = false, .clear_rgba = 0x8CBDF2FF}, argc, argv);
}
