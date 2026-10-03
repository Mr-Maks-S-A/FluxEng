/**
 * @example 02_bus_events.cpp
 * События шины EventSystem в журнале: записать то, что произошло за прогон, и воспроизвести в другую шину.
 *
 * Так делаются повторы событий, отладка («что происходило перед сбоем») и сетевая синхронизация по журналу.
 * Адаптер (`BusAdapter.hpp`) — единственное место, знающее про EventSystem.
 */

#include <EventLog/BusAdapter.hpp>

#include <cstdio>

using namespace EventLog;
namespace es = EventSystem;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

struct HitEvent {
    std::int32_t target = 0, damage = 0;
    static constexpr std::string_view event_name = "demo.hit";
    using fields = es::Fields<es::Field<"target", &HitEvent::target>, es::Field<"damage", &HitEvent::damage>>;
};
struct HealEvent {
    std::int32_t target = 0, amount = 0;
    static constexpr std::string_view event_name = "demo.heal";
    using fields = es::Fields<es::Field<"target", &HealEvent::target>, es::Field<"amount", &HealEvent::amount>>;
};

int main() {
    // 1. Шина игры: модуль «Combat» создаёт события, модуль «Journal» их читает.
    es::EventBus bus;
    const es::ModuleId combat = bus.declare_module("Combat").produces<HitEvent>().produces<HealEvent>();
    const es::ModuleId journal = bus.declare_module("Journal").consumes<HitEvent>().consumes<HealEvent>();
    auto hits_out = bus.writer<HitEvent>(combat);
    auto heals_out = bus.writer<HealEvent>(combat);
    auto hits_in = bus.reader<HitEvent>(journal);
    auto heals_in = bus.reader<HealEvent>(journal);

    // 2. Прогон на 30 тиков с записью: события тика N читатель видит после advance_tick() — их и пишем.
    MemoryStorage storage;
    int emitted = 0;
    {
        auto writer = Writer::create(storage, {.data_blocks = 4, .parity_blocks = 2, .block_size = 256}).value();
        for (std::uint32_t tick = 0; tick < 30; ++tick) {
            if (tick % 2 == 0) (void)hits_out.emit(HitEvent{static_cast<std::int32_t>(tick), 10 + static_cast<std::int32_t>(tick)}), ++emitted;
            if (tick % 3 == 0) (void)heals_out.emit(HealEvent{static_cast<std::int32_t>(tick), 5}), ++emitted;
            bus.advance_tick();
            record(writer, tick, hits_in);  // в журнал: все события тика, каждое — отдельной записью
            record(writer, tick, heals_in);
        }
    }
    const auto read = read_all(storage).value();
    std::printf("1. событий выпущено %d, записей в журнале %zu\n", emitted, read.records.size());
    EXPECT(read.records.size() == static_cast<std::size_t>(emitted) && read.report.clean());

    // 3. Воспроизведение в другую шину: те же события в те же тики, как будто игра прошла заново.
    es::EventBus replay_bus;
    const es::ModuleId source = replay_bus.declare_module("Replay").produces<HitEvent>().produces<HealEvent>();
    const es::ModuleId sink = replay_bus.declare_module("Sink").consumes<HitEvent>().consumes<HealEvent>();
    auto replay_hits_out = replay_bus.writer<HitEvent>(source);
    auto replay_heals_out = replay_bus.writer<HealEvent>(source);
    auto replay_hits_in = replay_bus.reader<HitEvent>(sink);
    auto replay_heals_in = replay_bus.reader<HealEvent>(sink);

    Replayer replayer(read.records);
    int hits_seen = 0, heals_seen = 0;
    for (std::uint32_t tick = 0; tick < 30; ++tick) {
        replayer.begin_tick(tick);
        (void)replayer.emit(replay_hits_out);
        (void)replayer.emit(replay_heals_out);
        replay_bus.advance_tick();
        for (const HitEvent& h : replay_hits_in.events()) {
            EXPECT(h.target == static_cast<std::int32_t>(tick) && h.damage == 10 + static_cast<std::int32_t>(tick));
            ++hits_seen;
        }
        heals_seen += static_cast<int>(replay_heals_in.events().size());
    }
    std::printf("2. воспроизведено: ударов %d, лечений %d (ожидалось 15 и 10)\n", hits_seen, heals_seen);
    EXPECT(hits_seen == 15 && heals_seen == 10);
    std::printf("OK\n");
    return 0;
}
