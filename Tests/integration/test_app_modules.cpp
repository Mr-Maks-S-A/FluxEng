/**
 * @file test_app_modules.cpp
 * @brief Core::App + RuntimeSystem: модули движка внутри клиента (скрытое окно, OpenGL).
 *
 * - Game::configure() добавляет модули, App::run() инициализирует их до Game::setup() и вызывает их хуки кадра и тика
 *   раньше Game::frame/Game::tick;
 * - завершение — в обратном порядке: Game::shutdown, затем модули (зависимые раньше зависимостей);
 * - исключение в тике не оставляет ни игру, ни модули без shutdown();
 * - сбой init() модуля откатывает уже поднятые модули, Game::setup() и цикл не запускаются.
 */

#include <Core/Core.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rs = RuntimeSystem;

namespace {

using Log = std::vector<std::string>;

class LoggedModule final : public rs::Module {
public:
    LoggedModule(std::string name, Log& log, std::vector<std::string> dependencies = {}, bool fail_init = false)
        : m_name(std::move(name)), m_log(log), m_fail_init(fail_init) {
        for (const std::string& d : dependencies) depends_on(d);
    }
    std::string_view name() const noexcept override { return m_name; }
    void init(rs::Runtime&) override {
        m_log.push_back(m_name + ".init");
        if (m_fail_init) throw std::runtime_error(m_name + " init failed");
    }
    void frame(rs::Runtime&, float) override { note("frame"); }
    void tick(rs::Runtime&) override { note("tick"); }
    void shutdown(rs::Runtime&) noexcept override { m_log.push_back(m_name + ".shutdown"); }

private:
    void note(const char* what) {
        if (m_log.size() < 24) m_log.push_back(m_name + "." + what);
    }
    std::string m_name;
    Log& m_log;
    bool m_fail_init;
};

class ModuleGame final : public Core::Game {
public:
    explicit ModuleGame(Log& log, int throw_at_tick = -1, bool fail_module_init = false)
        : m_log(log), m_throw_at(throw_at_tick), m_fail(fail_module_init) {}

    void configure(Core::App& app) override {
        m_log.push_back("game.configure");
        // Порядок add() — обратный зависимостям.
        app.add_module<LoggedModule>("Magic", m_log, std::vector<std::string>{"Voxel"});
        app.add_module<LoggedModule>("Voxel", m_log, std::vector<std::string>{}, m_fail);
    }
    void setup(Core::App&) override { m_log.push_back("game.setup"); }
    void frame(Core::App&, float) override { note("game.frame"); }
    void tick(Core::App& app) override {
        note("game.tick");
        if (m_throw_at >= 0 && static_cast<int>(app.tick()) == m_throw_at) throw std::runtime_error("tick failed");
    }
    void shutdown(Core::App&) override { m_log.push_back("game.shutdown"); }

private:
    void note(const char* what) {
        if (m_log.size() < 24) m_log.push_back(what);
    }
    Log& m_log;
    int m_throw_at;
    bool m_fail;
};

std::optional<Core::App> open_app(int ticks) {
    try {
        return std::optional<Core::App>(std::in_place, Core::AppConfig{.title = "AppModules", .width = 160, .height = 120, .visible = false,
                                                                         .max_ticks = ticks, .threads = 0});
    } catch (const std::exception& error) {
        MESSAGE("Core::App cannot start here (" << error.what() << ") — GPU test skipped");
        return std::nullopt;
    }
}

int index_of(const Log& log, std::string_view entry) {
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i] == entry) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

TEST_SUITE("Core + RuntimeSystem modules") {
    TEST_CASE("gpu: modules are configured, initialized before setup, run before the game and shut down after it") {
        auto app = open_app(3);
        if (!app) return;
        Log log;
        ModuleGame game(log);
        CHECK(app->run(game) == 0);

        const Log expected_prefix{"game.configure", "Voxel.init", "Magic.init", "game.setup", "Voxel.frame", "Magic.frame",
                                  "game.frame",     "Voxel.tick", "Magic.tick", "game.tick"};
        REQUIRE(log.size() >= expected_prefix.size());
        CHECK(Log(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(expected_prefix.size())) == expected_prefix);
        // Завершение: игра, затем модули — зависимое раньше зависимости.
        CHECK(Log(log.end() - 3, log.end()) == Log{"game.shutdown", "Magic.shutdown", "Voxel.shutdown"});
        CHECK(app->runtime().phase() == rs::Phase::Stopped);
    }

    TEST_CASE("gpu: an exception in the game tick still runs Game::shutdown and every Module::shutdown") {
        auto app = open_app(100);
        if (!app) return;
        Log log;
        ModuleGame game(log, /*throw_at_tick=*/2);
        CHECK_THROWS_AS(app->run(game), std::runtime_error);
        CHECK(index_of(log, "game.shutdown") >= 0);
        CHECK(index_of(log, "game.shutdown") < index_of(log, "Magic.shutdown"));
        CHECK(index_of(log, "Magic.shutdown") < index_of(log, "Voxel.shutdown"));
        CHECK(app->runtime().phase() == rs::Phase::Stopped);
    }

    TEST_CASE("gpu: a module whose init() fails stops startup: no setup, no frames, nothing left initialized") {
        auto app = open_app(3);
        if (!app) return;
        Log log;
        ModuleGame game(log, -1, /*fail_module_init=*/true); // падает Voxel — первый по зависимостям
        CHECK_THROWS_AS(app->run(game), std::runtime_error);
        CHECK(index_of(log, "game.setup") < 0);
        CHECK(index_of(log, "game.shutdown") < 0); // setup не состоялся — и shutdown игры не нужен
        CHECK(index_of(log, "Magic.init") < 0);
        CHECK(index_of(log, "Voxel.shutdown") < 0); // сам убирает за собой
    }
}
