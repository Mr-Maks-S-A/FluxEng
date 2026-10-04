#include "Probe.hpp"

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

using namespace RuntimeSystem;
using probe::Behavior;
using probe::Log;
using probe::Probe;

namespace {
RuntimeConfig quiet() { return {.threads = 0, .tick_arena_bytes = MemorySystem::MiB(4), .frame_arena_bytes = MemorySystem::MiB(4)}; }
} // namespace

TEST_SUITE("RuntimeSystem.Modules") {

TEST_CASE("без зависимостей порядок — порядок add()") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    rt.add<Probe>("B", log);
    rt.add<Probe>("C", log);
    rt.initialize();
    CHECK(rt.initialization_order() == std::vector<std::string_view>{"A", "B", "C"});
}

TEST_CASE("зависимость инициализируется раньше, хоть и добавлена позже") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("Combat", log, std::vector<std::string>{"Physics"});
    rt.add<Probe>("Physics", log);
    rt.add<Probe>("Ui", log, std::vector<std::string>{"Combat"});
    rt.initialize();
    CHECK(rt.initialization_order() == std::vector<std::string_view>{"Physics", "Combat", "Ui"});
    CHECK(probe::index_of(log, "Physics.init") < probe::index_of(log, "Combat.init"));
    CHECK(probe::index_of(log, "Combat.init") < probe::index_of(log, "Ui.init"));
}

TEST_CASE("ромб зависимостей: общий корень один раз и раньше всех") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("Top", log, std::vector<std::string>{"Left", "Right"});
    rt.add<Probe>("Left", log, std::vector<std::string>{"Root"});
    rt.add<Probe>("Right", log, std::vector<std::string>{"Root"});
    rt.add<Probe>("Root", log);
    rt.initialize();
    CHECK(rt.initialization_order() == std::vector<std::string_view>{"Root", "Left", "Right", "Top"});
}

TEST_CASE("declare() всех модулей идёт до init() первого") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    rt.add<Probe>("B", log);
    rt.initialize();
    CHECK(probe::index_of(log, "B.declare") < probe::index_of(log, "A.init"));
}

TEST_CASE("shutdown() идёт в обратном порядке init()") {
    Log log;
    {
        Runtime rt(quiet());
        rt.add<Probe>("B", log, std::vector<std::string>{"A"});
        rt.add<Probe>("A", log);
        rt.initialize();
        rt.shutdown();
        CHECK(rt.phase() == Phase::Stopped);
    }
    CHECK(probe::index_of(log, "B.shutdown") < probe::index_of(log, "A.shutdown"));
}

TEST_CASE("shutdown() идемпотентен, деструктор не вызывает его второй раз") {
    Log log;
    {
        Runtime rt(quiet());
        rt.add<Probe>("A", log);
        rt.initialize();
        rt.shutdown();
        rt.shutdown();
    }
    CHECK(probe::count_of(log, "A.shutdown") == 1);
}

TEST_CASE("деструктор завершает работающий Runtime") {
    Log log;
    {
        Runtime rt(quiet());
        rt.add<Probe>("A", log);
        rt.initialize();
    }
    CHECK(probe::count_of(log, "A.shutdown") == 1);
}

TEST_CASE("Runtime без initialize(): ни один хук не вызван, shutdown не нужен") {
    Log log;
    {
        Runtime rt(quiet());
        rt.add<Probe>("A", log);
    }
    CHECK(log.empty());
}

TEST_CASE("исключение в тике: состояние Running, shutdown() всё равно будет один раз") {
    Log log;
    {
        Runtime rt(quiet());
        rt.add<Probe>("A", log, std::vector<std::string>{}, Behavior{.throw_in_tick = true});
        rt.initialize();
        CHECK_THROWS_AS(rt.tick(), std::runtime_error);
        CHECK(rt.running());
    }
    CHECK(probe::count_of(log, "A.shutdown") == 1);
}

TEST_CASE("init() бросил: уже поднятые модули откатываются в обратном порядке, остальные не трогаются") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    rt.add<Probe>("B", log, std::vector<std::string>{"A"});
    rt.add<Probe>("Bad", log, std::vector<std::string>{"B"}, Behavior{.throw_in_init = true});
    rt.add<Probe>("Never", log, std::vector<std::string>{"Bad"});

    CHECK_THROWS_WITH_AS(rt.initialize(), "Bad init failed", std::runtime_error);
    CHECK(rt.phase() == Phase::Stopped);
    CHECK(probe::count_of(log, "Never.init") == 0);
    CHECK(probe::count_of(log, "Bad.shutdown") == 0); // сам убирает за собой
    CHECK(probe::index_of(log, "B.shutdown") < probe::index_of(log, "A.shutdown"));
    CHECK(probe::count_of(log, "A.shutdown") == 1);
    CHECK(probe::count_of(log, "Never.shutdown") == 0);

    log.clear();
    rt.shutdown(); // повторно ничего не делает
    CHECK(log.empty());
}

TEST_CASE("declare() бросил: ни один init() не вызван и ни один shutdown()") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    rt.add<Probe>("B", log, std::vector<std::string>{}, Behavior{.throw_in_declare = true});
    CHECK_THROWS_AS(rt.initialize(), std::runtime_error);
    CHECK(probe::count_of(log, "A.init") == 0);
    CHECK(probe::count_of(log, "A.shutdown") == 0);
    CHECK(rt.phase() == Phase::Stopped);
}

TEST_CASE("ошибки контракта") {
    Log log;
    SUBCASE("дубликат имени — сразу в add()") {
        Runtime rt(quiet());
        rt.add<Probe>("A", log);
        CHECK_THROWS_AS(rt.add<Probe>("A", log), RuntimeError);
        CHECK(rt.module_count() == 1);
    }
    SUBCASE("пустое имя") {
        Runtime rt(quiet());
        CHECK_THROWS_AS(rt.add<Probe>("", log), RuntimeError);
    }
    SUBCASE("неизвестная зависимость: хуки не вызваны, фаза Created") {
        Runtime rt(quiet());
        rt.add<Probe>("A", log, std::vector<std::string>{"Ghost"});
        CHECK_THROWS_WITH_AS(rt.initialize(), doctest::Contains("'Ghost'"), RuntimeError);
        CHECK(log.empty());
        CHECK(rt.phase() == Phase::Created);
    }
    SUBCASE("цикл называет участников") {
        Runtime rt(quiet());
        rt.add<Probe>("A", log, std::vector<std::string>{"B"});
        rt.add<Probe>("B", log, std::vector<std::string>{"A"});
        rt.add<Probe>("Free", log);
        CHECK_THROWS_WITH_AS(rt.initialize(), doctest::Contains("A, B"), RuntimeError);
        CHECK(log.empty());
    }
    SUBCASE("зависимость от самого себя — цикл") {
        Runtime rt(quiet());
        rt.add<Probe>("A", log, std::vector<std::string>{"A"});
        CHECK_THROWS_AS(rt.initialize(), RuntimeError);
    }
    SUBCASE("add() после initialize()") {
        Runtime rt(quiet());
        rt.initialize();
        CHECK_THROWS_AS(rt.add<Probe>("Late", log), RuntimeError);
    }
    SUBCASE("initialize() дважды") {
        Runtime rt(quiet());
        rt.initialize();
        CHECK_THROWS_AS(rt.initialize(), RuntimeError);
    }
    SUBCASE("tick()/update()/begin_frame() до initialize()") {
        Runtime rt(quiet());
        CHECK_THROWS_AS(rt.tick(), RuntimeError);
        CHECK_THROWS_AS(rt.update(0.1), RuntimeError);
        CHECK_THROWS_AS(rt.begin_frame(0.1), RuntimeError);
    }
    SUBCASE("tick() после shutdown()") {
        Runtime rt(quiet());
        rt.initialize();
        rt.shutdown();
        CHECK_THROWS_AS(rt.tick(), RuntimeError);
    }
}

TEST_CASE("find() по имени и по типу") {
    Log log;
    Runtime rt(quiet());
    Probe& a = rt.add<Probe>("A", log);
    CHECK(rt.find("A") == &a);
    CHECK(rt.find("B") == nullptr);
    CHECK(rt.find<Probe>() == &a);
}

TEST_CASE("фазы") {
    Log log;
    Runtime rt(quiet());
    CHECK(rt.phase() == Phase::Created);
    rt.add<Probe>("A", log);
    rt.initialize();
    CHECK(rt.phase() == Phase::Running);
    rt.shutdown();
    CHECK(rt.phase() == Phase::Stopped);
}

TEST_CASE("зависимость жива в shutdown() зависимого") {
    struct Provider final : Module {
        bool alive = false;
        std::string_view name() const noexcept override { return "Provider"; }
        void init(Runtime&) override { alive = true; }
        void shutdown(Runtime&) noexcept override { alive = false; }
    };
    struct Client final : Module {
        explicit Client(Provider& p) : provider(p) { depends_on("Provider"); }
        Provider& provider;
        bool provider_alive_at_shutdown = false;
        std::string_view name() const noexcept override { return "Client"; }
        void shutdown(Runtime&) noexcept override { provider_alive_at_shutdown = provider.alive; }
    };
    Runtime rt(quiet());
    Provider& provider = rt.add<Provider>();
    Client& client = rt.add<Client>(provider);
    rt.initialize();
    rt.shutdown();
    CHECK(client.provider_alive_at_shutdown);
    CHECK_FALSE(provider.alive);
}

} // TEST_SUITE
