/**
 * @file 01_headless_server.cpp
 * @brief Выделенный сервер без окна и GPU: три модуля, события между ними, цикл Runtime::run().
 *
 * - `Spawner` раз в 10 тиков «рождает» врага (событие SpawnEvent);
 * - `World` читает рождения и ведёт счёт живых (зависит от Spawner — получает его события тиком позже);
 * - `Stats` выводит итоги в shutdown().
 *
 * Тот же набор модулей без изменений работает внутри клиента (Core::App → app.runtime().add<…>()).
 */

#include <RuntimeSystem/RuntimeSystem.hpp>

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace es = EventSystem;
namespace rs = RuntimeSystem;

struct SpawnEvent {
    std::uint32_t id = 0;
    static constexpr std::string_view event_name = "game.spawn";
    using fields = es::Fields<es::Field<"id", &SpawnEvent::id>>;
};

class Spawner final : public rs::Module {
public:
    std::string_view name() const noexcept override { return "Spawner"; }
    void declare(rs::Runtime& rt) override { m_id = rt.bus().declare_module("Spawner").produces<SpawnEvent>(); }
    void init(rs::Runtime& rt) override { m_out = rt.bus().writer<SpawnEvent>(m_id); }
    void tick(rs::Runtime& rt) override {
        if (rt.current_tick() % 10 == 0) m_out.emit(SpawnEvent{++m_next_id});
    }

private:
    es::ModuleId m_id{};
    es::EventWriter<SpawnEvent> m_out;
    std::uint32_t m_next_id = 0;
};

class World final : public rs::Module {
public:
    World() { depends_on("Spawner"); }
    std::string_view name() const noexcept override { return "World"; }
    void declare(rs::Runtime& rt) override { m_id = rt.bus().declare_module("World").consumes<SpawnEvent>(); }
    void init(rs::Runtime& rt) override { m_in = rt.bus().reader<SpawnEvent>(m_id); }
    void tick(rs::Runtime&) override {
        for (const SpawnEvent& spawn : m_in.events()) {
            (void)spawn;
            ++alive;
        }
    }
    int alive = 0;

private:
    es::ModuleId m_id{};
    es::EventReader<SpawnEvent> m_in;
};

class Stats final : public rs::Module {
public:
    explicit Stats(const World& world) : m_world(world) { depends_on("World"); }
    std::string_view name() const noexcept override { return "Stats"; }
    // Зависимость (World) ещё жива: Stats завершается раньше неё.
    void shutdown(rs::Runtime& rt) noexcept override {
        std::printf("итог: тиков %llu, живых врагов %d\n", static_cast<unsigned long long>(rt.current_tick()), m_world.alive);
    }

private:
    const World& m_world;
};

int main() {
    rs::Runtime server({.ticks_per_second = 60.0, .threads = 2, .profile_modules = true});

    rs::Module& world = server.add<World>();
    server.add<Stats>(static_cast<const World&>(world));
    server.add<Spawner>(); // порядок add() не важен: порядок init задают зависимости

    server.initialize();
    std::printf("порядок инициализации:");
    for (const std::string_view name : server.initialization_order()) std::printf(" %.*s", static_cast<int>(name.size()), name.data());
    std::printf("\n");

    // 600 тиков без ожидания по часам — «прогнать вперёд». Для настоящего сервера: realtime = true
    // и request_stop() из обработчика сигнала.
    const int ticks = server.run({.max_ticks = 600, .realtime = false});

    for (const rs::ModuleStats& stats : server.module_stats()) {
        std::printf("  %-8.*s тиков %llu, в среднем %llu нс\n", static_cast<int>(stats.name.size()), stats.name.data(),
                    static_cast<unsigned long long>(stats.ticks), static_cast<unsigned long long>(stats.total_ns / (stats.ticks ? stats.ticks : 1)));
    }
    server.shutdown(); // можно и не вызывать: деструктор сделает то же самое
    return ticks == 600 ? 0 : 1;
}
