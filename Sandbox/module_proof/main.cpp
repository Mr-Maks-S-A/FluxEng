/**
 * @file main.cpp
 * @brief ModuleProof — игра-проверка: каждый модуль движка используется в живом приложении, результат — чек-лист на экране и в консоли.
 *
 * Это не демонстрация геймплея, а **доказательство, что всё собирается и работает вместе**: игра линкуется с зонтичной целью
 * `engine::FluxEng` (все 22 модуля) и по одной проверке на тик прогоняет каждый из них на настоящих объектах — тех же вызовах,
 * что делают остальные игры. Если модуль перестал собираться, потерял зависимость или сломал контракт, ModuleProof не соберётся
 * или упадёт с названием модуля.
 *
 * Фазы жизни приложения (Core): `setup` — проверки, не требующие кадра; `tick` — по проверке на тик (так же проверяется,
 * что фиксированный тик, шина и память тика живы); `render_overlay` — чек-лист, карта высот (WorldRender), редактор рун
 * (RuneEditor + View) рисуются через Renderer2D, а в конце кадр сохраняется (`--screenshot`).
 *
 * Запуск: `ModuleProof` (окно с чек-листом) | `ModuleProof --ticks 120` (автовыход) | `ModuleProof --ticks 120 --screenshot proof.png`.
 * Код возврата 0 — все модули в порядке; в консоли последняя строка `ModuleProof: OK`.
 */

#include <Challenge/Play.hpp>
#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>
#include <EventLog/Journal.hpp>
#include <EventLog/Storage.hpp>
#include <EventSystem/EventSystem.hpp>
#include <JobSystem/JobSystem.hpp>
#include <ManaField/ManaField.hpp>
#include <Math/Crc32c.hpp>
#include <Math/Math.hpp>
#include <MemorySystem/MemorySystem.hpp>
#include <Net/Lockstep.hpp>
#include <Phases/Schedule.hpp>
#include <Replay/Replay.hpp>
#include <RuneEditor/Analysis.hpp>
#include <RuneEditor/Controller.hpp>
#include <RuneEditor/Editor.hpp>
#include <RuneEditor/Layout.hpp>
#include <RuneEditor/View.hpp>
#include <Runes/Runes.hpp>
#include <SpellSim/SpellSim.hpp>
#include <Terrain/Terrain.hpp>
#include <WorldRender/Adapters/Terrain.hpp>
#include <WorldRender/Overlay.hpp>
#include <WorldRender/WorldRender.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <numeric>
#include <print>
#include <string>
#include <vector>

namespace es = EventSystem;
using namespace RendererSystem;
namespace ed = RuneEditor;
using Math::Fixed;
using Math::WorldPos;

namespace {

int g_failed = 0;

/// Событие для проверки шины: объявляет всё о себе само.
struct ProofEvent {
    std::uint32_t value = 0;
    static constexpr std::string_view event_name = "proof.event";
    using fields = es::Fields<es::Field<"value", &ProofEvent::value>>;
};

struct Result {
    std::string module;
    std::string detail;
    bool ok = false;
};

class ModuleProof final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {1280.0f, 720.0f}; }

    void setup(Core::App& app) override {
        build_checks();
        // Карта высот и редактор — материал для экрана (WorldRender, RuneEditor).
        sim.emplace(SpellSim::Config{.seed = 3, .setup = {}});
        heights.emplace(128, 128, 1.0);
        height_source.emplace(sim->terrain());
        heights->update(*height_source);
        const auto [low, high] = heights->range();
        height_tex = app.renderer().create_texture(heights->shade({.exaggeration = 2.0, .low = low, .high = high}), {.filter = TextureFilter::Linear});
        editor_view.emplace(app.renderer());
        build_demo_graph();
        std::println("ModuleProof: {} проверок, по одной на тик", checks.size());
    }

    void tick(Core::App& app) override {
        if (next < checks.size()) {
            Check& c = checks[next++];
            Result r{c.module, {}, false};
            try {
                r.ok = c.run(app, r.detail);
            } catch (const std::exception& e) {
                r.detail = std::string("исключение: ") + e.what();
            }
            if (!r.ok) ++g_failed;
            std::println("  [{}] {:<14} {}", r.ok ? "ok" : "ОШИБКА", r.module, r.detail);
            results.push_back(std::move(r));
        } else if (!finished) {
            finished = true;
            if (g_failed == 0) std::println("ModuleProof: OK ({} из {} модулей)", results.size(), checks.size());
            else std::println("ModuleProof: ОШИБКА ({} из {})", g_failed, checks.size());
        } // дальше окно остаётся с чек-листом; выход — по `--ticks N` (Core) или вручную
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        const glm::vec2 vp = app.camera().viewport;
        const FontHandle font = app.ui_font();
        r.fill_rect({{0, 0}, vp}, Color{10, 12, 22, 255}, -10);

        // Чек-лист.
        std::vector<std::string> lines{std::format("ModuleProof — тик {}, проверено {}/{}", app.tick(), results.size(), checks.size())};
        for (const Result& res : results) lines.push_back(std::format("{} {:<13} {}", res.ok ? "+" : "!", res.module, res.detail));
        WorldRender::overlay::panel(r, font, {10.0f, 10.0f}, lines, 15.0f);

        // WorldRender: карта высот SDF-мира; ManaField — тем же слоем не рисуем, достаточно проверки.
        const float side = std::min(vp.x * 0.30f, vp.y * 0.45f);
        const glm::vec2 map_at{vp.x - side - 20.0f, 20.0f};
        r.draw({.position = map_at, .size = {side, side}, .pivot = {0, 0}, .texture = height_tex, .layer = 0});
        r.draw_rect({map_at, {side, side}}, 2.0f, Color{255, 255, 255, 90}, 1);
        r.draw_text(font, "WorldRender: HeightMap", map_at + glm::vec2{4.0f, side + 4.0f}, {.size = 14.0f, .layer = 5});

        // RuneEditor + View: граф «вырезать шар» собран кодом (Editor::execute), нарисован View.
        const Rect area{{vp.x - side - 20.0f, side + 50.0f}, {side, vp.y - side - 70.0f}};
        const ed::Marks marks{.activity = nullptr, .analysis = &analysis, .locked = false};
        editor_view->draw(r, font, editor, controller, marks, area, 20);
        (void)app;
    }

    [[nodiscard]] std::string status() const override { return std::format("{}/{} проверок", results.size(), checks.size()); }

    void shutdown(Core::App&) override {
        if (!finished) std::println("ModuleProof: остановлено до конца ({} из {} проверок)", results.size(), checks.size());
    }

private:
    struct Check {
        std::string module;
        std::function<bool(Core::App&, std::string&)> run;
    };

    void add(std::string module, std::function<bool(Core::App&, std::string&)> run) { checks.push_back({std::move(module), std::move(run)}); }

    void build_demo_graph() {
        const Runes::NodeId target = editor.add_node(Runes::Rune::Target, {40, 60});
        const Runes::NodeId radius = editor.add_node(Runes::Rune::Push, {40, 160}, Fixed::from_int(2).raw);
        const Runes::NodeId carve = editor.add_node(Runes::Rune::Carve, {240, 90});
        const Runes::NodeId halt = editor.add_node(Runes::Rune::Halt, {420, 90});
        (void)editor.connect({target, ed::PortKind::Output, 0}, {carve, ed::PortKind::Input, 0});
        (void)editor.connect({radius, ed::PortKind::Output, 0}, {carve, ed::PortKind::Input, 1});
        (void)editor.connect({carve, ed::PortKind::Next, 0}, {halt, ed::PortKind::Input, 0});
        editor.set_entry(carve);
        analysis = ed::analyze(editor.graph());
        controller.camera.zoom = 0.45f;
    }

    void build_checks() {
        // ---- основа ----
        add("Math", [](Core::App&, std::string& d) {
            const Fixed a = Fixed::from_ratio(1, 3), b = Fixed::from_int(3);
            Math::Rng r1(5), r2(5);
            const std::byte nine[] = {std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'}, std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
            const bool crc = Math::crc32c(nine) == 0xE3069283u; // эталонный вектор CRC-32C
            const bool ok = (a * b).raw >= Fixed::one_raw - 2 && r1.next() == r2.next() && Math::isqrt(144) == 12 && crc;
            d = std::format("Fixed 1/3·3 = {:.4f}, isqrt(144) = 12, Rng воспроизводим, CRC-32C = эталон", (a * b).to_double());
            return ok;
        });
        add("MemorySystem", [](Core::App& app, std::string& d) {
            MemorySystem::Arena& arena = app.tick_arena();
            const std::size_t before = arena.used();
            std::uint32_t* values = arena.push_array<std::uint32_t>(64).data();
            values[63] = 7;
            auto pool = MemorySystem::Pool<std::uint64_t>::reserve(128);
            std::uint64_t* a = pool.allocate();
            *a = 5;
            pool.free(a);
            std::uint64_t* b = pool.allocate(); // освобождённый блок переиспользуется и обнулён
            const bool ok = arena.used() > before && values[0] == 0 && b == a && *b == 0;
            d = std::format("арена тика +{} Б (ZII), пул переиспользует блок", arena.used() - before);
            return ok;
        });
        add("JobSystem", [](Core::App& app, std::string& d) {
            std::vector<std::uint64_t> data(100000);
            std::iota(data.begin(), data.end(), 1ull);
            const auto sum = [&](JobSystem::Scheduler& jobs) {
                return JobSystem::parallel_reduce(jobs, data.size(), 4096, std::uint64_t{0},
                    [&](std::size_t lo, std::size_t hi) { std::uint64_t s = 0; for (std::size_t i = lo; i < hi; ++i) s += data[i]; return s; },
                    [](std::uint64_t x, std::uint64_t y) { return x + y; });
            };
            JobSystem::Scheduler serial(JobSystem::SchedulerConfig{.threads = 0});
            const std::uint64_t parallel = sum(app.jobs()), single = sum(serial);
            d = std::format("параллельная сумма = {} (одна нить: то же), потоков {}", parallel, app.jobs().concurrency());
            return parallel == 5000050000ull && single == parallel;
        });
        add("Phases", [](Core::App&, std::string& d) {
            std::string order;
            Phases::Schedule schedule;
            schedule.add("a", [&] { order += 'a'; }).add("c", [&] { order += 'c'; });
            const bool inserted = schedule.insert_after("a", "b", [&] { order += 'b'; });
            schedule.run();
            d = std::format("порядок фаз «{}», замеров {}", order, schedule.times().size());
            return inserted && order == "abc" && schedule.times().size() == 3;
        });
        add("EventSystem", [](Core::App&, std::string& d) {
            es::EventBus bus;
            bus.register_event<ProofEvent>();
            auto out = bus.writer<ProofEvent>();
            auto in = bus.reader<ProofEvent>();
            out.emit(ProofEvent{41});
            const bool hidden = in.empty(); // событие видно только в следующем тике
            bus.advance_tick();
            const bool seen = !in.empty() && in.events()[0].value == 41;
            d = std::format("событие тика N читается в тике N+1 (до: {}, после: {})", hidden ? "скрыто" : "видно", seen ? "видно" : "нет");
            return hidden && seen;
        });
        add("ECSSystem", [](Core::App&, std::string& d) {
            ECS::World world;
            const ECS::Entity e = world.create();
            world.destroy(e);
            const ECS::Entity fresh = world.create();
            d = std::format("слот сущности переиспользован ({}), старая ссылка недействительна", fresh.index == e.index ? "да" : "нет");
            return !world.valid(e) && world.valid(fresh) && fresh.generation != e.generation;
        });
        add("EventLog", [](Core::App&, std::string& d) {
            EventLog::MemoryStorage storage;
            const EventLog::Config config{.data_blocks = 4, .parity_blocks = 2, .block_size = 256};
            {
                auto writer = EventLog::Writer::create(storage, config).value();
                for (std::uint32_t t = 0; t < 200; ++t) writer.append(t, ProofEvent{t});
                if (!writer.flush()) return false;
            }
            storage.bytes()[64 + 40] ^= std::byte{0xFF}; // порча байта в первом блоке
            const auto read = EventLog::read_all(storage);
            if (!read) return false;
            d = std::format("200 записей, порча блока исправлена по чётности (восстановлено {})", read->report.blocks_repaired);
            return read->records.size() == 200 && read->report.blocks_repaired >= 1;
        });
        add("Replay", [](Core::App&, std::string& d) {
            auto session = Replay::Session::record(9).value();
            const Replay::Command cmd{.type = 1, .x = 5};
            (void)session.begin_tick(0, std::span(&cmd, 1));
            Replay::StateHashes hashes;
            hashes.add("sum", 77);
            if (!session.finish(2, hashes)) return false;
            auto replay = Replay::Session::replay(session.recording());
            const bool same = replay.begin_tick(0, {}).size() == 1;
            const auto verdict = replay.finish(2, hashes);
            d = std::format("запись → повтор: команда та же, хеши {}", verdict && verdict->match ? "совпали" : "РАЗОШЛИСЬ");
            return same && verdict && verdict->match;
        });
        add("Net", [](Core::App&, std::string& d) {
            Net::LoopbackNetwork network(2, {.latency_steps = 2, .jitter_steps = 2, .loss_permille = 200, .duplicate_permille = 80}, 4);
            Net::Lockstep a(network.endpoint(0), {.local = 0, .peers = 2, .seed = 1}), b(network.endpoint(1), {.local = 1, .peers = 2, .seed = 1});
            std::int64_t sum_a = 0, sum_b = 0;
            std::uint32_t ticks_a = 0, ticks_b = 0;
            for (int i = 0; i < 4000 && (ticks_a < 100 || ticks_b < 100); ++i) {
                for (auto [net, sum, ticks, tag] : {std::tuple<Net::Lockstep*, std::int64_t*, std::uint32_t*, int>{&a, &sum_a, &ticks_a, 1}, {&b, &sum_b, &ticks_b, 2}}) {
                    net->pump();
                    if (net->needs_input()) {
                        const Replay::Command c{.type = 1, .x = tag * 10};
                        net->submit(std::span(&c, 1));
                    }
                    if (*ticks < 100 && net->ready()) {
                        for (const Replay::Command& c : net->advance()) *sum += c.x;
                        ++*ticks;
                    }
                }
                network.step();
            }
            d = std::format("2 пира, потери 20 %: сумма ввода {} = {} на тике {}", sum_a, sum_b, ticks_a);
            return ticks_a == 100 && ticks_b == 100 && sum_a == sum_b && sum_a > 0;
        });
        // ---- мир ----
        add("ManaField", [](Core::App&, std::string& d) {
            ManaField::ManaGrid grid;
            const WorldPos at = WorldPos::from_meters(64, 10, 64);
            const Math::Mana before = grid.density(at);
            const Math::Mana got = grid.draw(at, Fixed::from_int(3), Math::Mana::from_int(240));
            const Math::Mana hole = grid.density(at);
            for (int i = 0; i < 300; ++i) grid.step();
            const Math::Mana healed = grid.density(at);
            d = std::format("взято {:.0f}, плотность {:.0f} → {:.1f} → {:.1f} (дыра затягивается)", got.to_double(), before.to_double(), hole.to_double(), healed.to_double());
            return got.to_double() > 239.0 && hole < before && healed > hole;
        });
        add("Terrain", [](Core::App&, std::string& d) {
            Terrain::SdfWorld world(2);
            const std::int64_t cx = world.layout().size_x() / 2, cz = world.layout().size_z() / 2;
            const WorldPos surface{cx, world.ground_height(cx, cz), cz};
            const Fixed before = world.sample({cx, surface.y - Fixed::one_raw, cz});
            (void)world.carve_sphere(surface, Fixed::from_int(3));
            const Fixed after = world.sample({cx, surface.y - Fixed::one_raw, cz});
            const auto hit = world.raycast({cx, surface.y + 8 * Fixed::one_raw, cz}, {Fixed{}, Fixed::from_int(-1), Fixed{}}, Fixed::from_int(40));
            d = std::format("SDF: под поверхностью {:.2f} → {:.2f} после выреза, луч {}", before.to_double(), after.to_double(), hit ? "попал" : "мимо");
            return before < Fixed{} && after > Fixed{} && hit.has_value();
        });
        add("Runes", [](Core::App&, std::string& d) {
            const auto program = Runes::parse_program("TARGET\nPUSH 2\nCARVE\nHALT\n");
            if (!program) return false;
            const auto bytes = Runes::encode_program(*program);
            const auto back = Runes::decode_program(bytes);
            const Runes::CostEstimate cost = Runes::estimate_cost(*program);
            d = std::format("разбор → байты ({}) → разбор, цена прохода {:.0f} маны", bytes.size(), cost.per_pass.to_double());
            return back && back->code.size() == program->code.size() && cost.effects == 1 && cost.per_pass.to_double() > 200.0;
        });
        add("Character", [](Core::App&, std::string& d) {
            SpellSim::Simulation s;
            const WorldPos start = s.world().get<Character::Position>(s.player())->value;
            const Replay::Command go = SpellSim::move_command(Fixed::from_int(1), Fixed{});
            s.tick(std::span(&go, 1));
            for (int i = 0; i < 59; ++i) s.tick({});
            const WorldPos now = s.world().get<Character::Position>(s.player())->value;
            d = std::format("за секунду ходьбы маг прошёл {:.1f} м по SDF-земле", static_cast<double>(now.x - start.x) / Fixed::one_raw);
            return now.x - start.x > 3 * Fixed::one_raw;
        });
        add("SpellSim", [](Core::App&, std::string& d) {
            const auto run = [] {
                SpellSim::Simulation s(SpellSim::Config{.seed = 11, .setup = {}});
                (void)s.programs().add_text("carve", "TARGET\nPUSH 2\nCARVE\nHALT\n");
                s.set_grimoire_slot(0, "carve");
                const Replay::Command cast = SpellSim::cast_command(0, Runes::ManaSource::Personal, {Fixed{}, Fixed::from_int(-1), Fixed{}});
                for (int i = 0; i < 120; ++i) s.tick(i == 30 ? std::span(&cast, 1) : std::span<const Replay::Command>{});
                return std::pair{s.hashes(), s.edits_applied()};
            };
            const auto [h1, e1] = run();
            const auto [h2, e2] = run();
            d = std::format("два прогона из одного сида: хеши {}, правок земли {}; {}", h1 == h2 ? "совпали" : "РАЗОШЛИСЬ", e1, h1.describe().substr(0, 40));
            return h1 == h2 && e1 == 1 && e2 == 1;
        });
        add("Challenge", [](Core::App&, std::string& d) {
            const Challenge::Level& level = *Challenge::find_level("mine");
            const Challenge::Outcome o = Challenge::play(level, *Challenge::reference_solution("mine"));
            d = std::format("уровень «{}»: {}", level.title, o.status_line);
            return o.status == Challenge::Status::Won && o.stars == 3;
        });
        // ---- редактор и показ ----
        add("RuneEditor", [this](Core::App&, std::string& d) {
            ed::Editor local;
            const Runes::NodeId n = local.add_node(Runes::Rune::Push, {0, 0}, Fixed::from_int(1).raw);
            const bool undo = local.undo() && local.graph().find(n) == nullptr;
            const bool redo = local.redo() && local.graph().find(n) != nullptr;
            d = std::format("демо-граф: {} узлов, собран: {}; undo/redo: {}", editor.graph().nodes().size(), analysis.ok() ? "да" : "нет", undo && redo ? "работают" : "СЛОМАНЫ");
            return analysis.ok() && undo && redo && analysis.cost.effects == 1;
        });
        add("WindowSystem", [](Core::App& app, std::string& d) {
            app.window().inject_key(GLFW_KEY_F9, GLFW_PRESS); // ввод без железа: путь окно → состояние ввода
            const bool delivered = app.window().input().down(GLFW_KEY_F9);
            app.window().inject_key(GLFW_KEY_F9, GLFW_RELEASE);
            d = std::format("окно {}×{}, поддельное нажатие клавиши {}", app.config().width, app.config().height, delivered ? "дошло" : "потеряно");
            return delivered;
        });
        add("RendererSystem", [](Core::App& app, std::string& d) {
            const TextureHandle t = app.renderer().create_texture(Procedural::circle_image(16, Colors::white), {.filter = TextureFilter::Linear});
            d = "текстура из процедурного изображения создана; кадр рисуется (чек-лист, карта, редактор — на экране)";
            return t != TextureHandle::white();
        });
        add("WorldRender", [this](Core::App&, std::string& d) {
            const auto [low, high] = heights->range();
            const double h = heights->height_at(64.0, 64.0);
            d = std::format("карта высот 128×128: диапазон {:.1f}…{:.1f} м, высота в центре {:.1f}", low, high, h);
            return high > low && h > low - 0.01 && h < high + 0.01;
        });
        add("Core", [](Core::App& app, std::string& d) {
            d = std::format("тик {} из App, шаг {:.4f} с, потоков {}", app.tick(), app.tick_seconds(), app.jobs().concurrency());
            return app.tick() > 0 && app.tick_seconds() > 0.0f;
        });
    }

    std::vector<Check> checks;
    std::vector<Result> results;
    std::size_t next = 0;
    bool finished = false;

    std::optional<SpellSim::Simulation> sim;
    std::optional<WorldRender::HeightMap> heights;
    std::optional<WorldRender::TerrainHeights> height_source;
    TextureHandle height_tex;
    std::optional<ed::View> editor_view;
    ed::Editor editor;
    ed::Controller controller{editor};
    ed::Analysis analysis;
};

} // namespace

int main(int argc, char** argv) {
    const int code = Core::run<ModuleProof>({.title = "ModuleProof", .ticks_per_second = 60.0, .camera_controls = false, .clear_rgba = 0x0A0C16FF}, argc, argv);
    return code != 0 ? code : (g_failed != 0 ? 1 : 0);
}
