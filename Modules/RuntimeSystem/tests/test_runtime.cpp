#include "Probe.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <vector>

using namespace RuntimeSystem;
using probe::Log;
using probe::Probe;
namespace es = EventSystem;

namespace {

struct Ping {
    std::uint32_t value = 0;
    static constexpr std::string_view event_name = "test.ping";
    using fields = es::Fields<es::Field<"value", &Ping::value>>;
};

RuntimeConfig quiet(double hz = 30.0) {
    return {.ticks_per_second = hz, .threads = 0, .tick_arena_bytes = MemorySystem::MiB(4), .frame_arena_bytes = MemorySystem::MiB(4)};
}

class Sender final : public Module {
public:
    std::string_view name() const noexcept override { return "Sender"; }
    void declare(Runtime& rt) override { m_id = rt.bus().declare_module("Sender").produces<Ping>(); }
    void init(Runtime& rt) override { m_out = rt.bus().writer<Ping>(m_id); }
    void tick(Runtime&) override { m_out.emit(Ping{++m_sent}); }
private:
    es::ModuleId m_id{};
    es::EventWriter<Ping> m_out;
    std::uint32_t m_sent = 0;
};

class Receiver final : public Module {
public:
    Receiver() { depends_on("Sender"); }
    std::string_view name() const noexcept override { return "Receiver"; }
    void declare(Runtime& rt) override { m_id = rt.bus().declare_module("Receiver").consumes<Ping>(); }
    void init(Runtime& rt) override { m_in = rt.bus().reader<Ping>(m_id); }
    void tick(Runtime&) override {
        for (const Ping& ping : m_in.events()) seen.push_back(ping.value);
    }
    std::vector<std::uint32_t> seen;
private:
    es::ModuleId m_id{};
    es::EventReader<Ping> m_in;
};

} // namespace

TEST_SUITE("RuntimeSystem.Runtime") {

TEST_CASE("зависимость задаёт порядок тика; события видны в следующем тике") {
    Runtime rt(quiet());
    rt.add<Receiver>(); // добавлен раньше отправителя — порядок задаст зависимость
    rt.add<Sender>();
    rt.initialize();
    for (int i = 0; i < 3; ++i) rt.tick();
    CHECK(rt.current_tick() == 3);
    CHECK(rt.find<Receiver>()->seen == std::vector<std::uint32_t>{1, 2}); // событие тика N — в тике N+1
}

TEST_CASE("обратный вызов игры идёт после всех модулей") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    rt.add<Probe>("B", log);
    rt.set_tick_callback([&](Runtime&) { log.push_back("game"); });
    rt.initialize();
    log.clear();
    rt.tick();
    CHECK(log == Log{"A.tick", "B.tick", "game"});
}

TEST_CASE("память тика: значение прошлого тика читается, позапрошлого — нет") {
    Runtime rt(quiet());
    rt.initialize();
    int* first = rt.tick_arena().push<int>();
    *first = 7;
    rt.tick();
    CHECK(rt.previous_tick_arena().owns(first));
    CHECK(*first == 7);
    rt.tick();
    CHECK_FALSE(rt.previous_tick_arena().owns(first));
}

TEST_CASE("begin_frame сбрасывает арену кадра и зовёт frame() модулей") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    rt.initialize();
    (void)rt.frame_arena().push(128);
    CHECK(rt.frame_arena().used() >= 128);
    rt.begin_frame(0.016);
    CHECK(rt.frame_arena().used() == 0);
    CHECK(probe::count_of(log, "A.frame") == 1);
}

TEST_CASE("update(): сколько тиков набежало; на паузе кадры идут, тиков нет") {
    Runtime rt(quiet(30.0));
    rt.initialize();
    CHECK(rt.update(0.1) == 3); // 3 × 33.3 мс
    CHECK(rt.current_tick() == 3);
    rt.step().paused = true;
    CHECK(rt.update(1.0) == 0);
    CHECK(rt.current_tick() == 3);
    CHECK(rt.bus().current_frame() == 2); // advance_frame — в каждом update
}

TEST_CASE("lockstep: один тик на update независимо от времени") {
    RuntimeConfig config = quiet();
    config.lockstep = true;
    Runtime rt(config);
    rt.initialize();
    CHECK(rt.update(10.0) == 1);
    CHECK(rt.update(0.0) == 1);
}

TEST_CASE("run() без ожидания: ровно max_ticks тиков") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log);
    CHECK(rt.run({.max_ticks = 25, .realtime = false}) == 25); // сам вызвал initialize()
    CHECK(probe::count_of(log, "A.tick") == 25);
    CHECK(rt.current_tick() == 25);
}

TEST_CASE("request_stop() из тика останавливает run() после этого тика") {
    struct Stopper final : Module {
        std::string_view name() const noexcept override { return "Stopper"; }
        void tick(Runtime& rt) override { if (rt.current_tick() == 9) rt.request_stop(); }
    };
    Runtime rt(quiet());
    rt.add<Stopper>();
    CHECK(rt.run({.max_ticks = -1, .realtime = false}) == 10);
    CHECK(rt.stop_requested());
}

TEST_CASE("run() в реальном времени держит темп и не уходит вперёд часов") {
    Runtime rt(quiet(200.0)); // 5 мс на тик
    const auto start = std::chrono::steady_clock::now();
    const int ticks = rt.run({.max_ticks = 20, .realtime = true});
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(ticks == 20);
    CHECK(elapsed >= 0.08); // 20 тиков по 5 мс = 100 мс; нижняя граница с запасом
}

TEST_CASE("run() после ошибки зависимостей пробрасывает её") {
    Log log;
    Runtime rt(quiet());
    rt.add<Probe>("A", log, std::vector<std::string>{"Ghost"});
    CHECK_THROWS_AS(rt.run({.max_ticks = 1, .realtime = false}), RuntimeError);
}

TEST_CASE("неверная частота тика — ошибка конструктора") {
    CHECK_THROWS_AS(Runtime(RuntimeConfig{.ticks_per_second = 0.0, .threads = 0}), RuntimeError);
}

TEST_CASE("статистика модулей: только если включена") {
    Log log;
    {
        Runtime rt(quiet());
        rt.add<Probe>("A", log);
        rt.run({.max_ticks = 3, .realtime = false});
        CHECK(rt.module_stats().empty());
    }
    RuntimeConfig config = quiet();
    config.profile_modules = true;
    Runtime rt(config);
    rt.add<Probe>("A", log);
    rt.add<Probe>("B", log);
    rt.run({.max_ticks = 4, .realtime = false});
    const auto stats = rt.module_stats();
    REQUIRE(stats.size() == 2);
    CHECK(stats[0].name == "A");
    CHECK(stats[0].ticks == 4);
    CHECK(stats[1].total_ns >= stats[1].max_ns);
}

TEST_CASE("детерминизм: результат параллельного модуля не зависит от числа потоков") {
    struct Summer final : Module {
        std::uint64_t checksum = 1469598103934665603ull;
        std::string_view name() const noexcept override { return "Summer"; }
        void tick(Runtime& rt) override {
            const std::uint64_t total = JobSystem::parallel_reduce(
                rt.jobs(), 100'000, 1024, std::uint64_t{0},
                [&](std::size_t b, std::size_t e) {
                    std::uint64_t s = 0;
                    for (std::size_t i = b; i < e; ++i) s += i * (rt.current_tick() + 1);
                    return s;
                },
                [](std::uint64_t a, std::uint64_t b) { return a + b; });
            checksum = (checksum ^ total) * 1099511628211ull;
        }
    };
    std::vector<std::uint64_t> results;
    for (const int threads : {0, 1, 4}) {
        RuntimeConfig config = quiet();
        config.threads = threads;
        Runtime rt(config);
        Summer& summer = rt.add<Summer>();
        rt.run({.max_ticks = 30, .realtime = false});
        results.push_back(summer.checksum);
    }
    CHECK(results[0] == results[1]);
    CHECK(results[0] == results[2]);
}

} // TEST_SUITE
