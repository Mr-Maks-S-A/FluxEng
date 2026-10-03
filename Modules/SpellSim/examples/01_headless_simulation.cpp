/**
 * @example 01_headless_simulation.cpp
 * Вся игра без окна: мир, персонаж, заклинания, поле маны — один объект `Simulation`, шаг за шагом.
 *
 * Показано: команды (типизированные), порядок фаз тика (расписание), события шины, запись и повтор с проверкой хешей,
 * подключение собственной фазы. Именно так работают тесты и `--replay` в игре.
 */

#include <SpellSim/SpellSim.hpp>

#include <cstdio>

using namespace SpellSim;
using Math::Fixed;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/// Простой сценарий «игрока»: команды по тикам. Реальный ввод превращается в такие же команды.
static std::vector<Replay::Command> input_at(std::uint32_t tick) {
    const Math::FVec3 down_forward = Math::quantize_direction(1.0, -1.0, 0.0); // взгляд вниз-вперёд (float → Fixed на границе)
    switch (tick) {
    case 10: return {MoveCommand{Fixed::from_int(1), Fixed{}}.encode()};
    case 40: return {CastCommand{0, Runes::ManaSource::Personal, down_forward}.encode()};
    case 90: return {JumpCommand{}.encode()};
    case 120: return {CastCommand{1, Runes::ManaSource::Ambient, down_forward}.encode()};
    default: return {};
    }
}

/// Один прогон на N тиков. Через Driver — тот же путь, что у игры (запись/повтор подключаются заменой сессии).
static Replay::StateHashes run(std::uint32_t ticks, Replay::Session& session, std::uint32_t* edits = nullptr) {
    Simulation sim(Config{.seed = 11});
    (void)sim.programs().add_text("carve", "TARGET\nPUSH 2\nCARVE\nHALT\n");
    (void)sim.programs().add_text("raise", "TARGET\nPUSH 2\nRAISE\nHALT\n");
    Replay::Driver driver(sim, session);
    for (std::uint32_t t = 0; t < ticks; ++t) {
        const std::vector<Replay::Command> live = input_at(t);
        if (!driver.step(live)) break;
    }
    if (edits) *edits = sim.edits_applied();
    return sim.hashes();
}

int main() {
    // 1. Один прогон. Хеши названы по подсистемам: terrain, mana, characters, spells, rng, tick.
    Replay::Session off = Replay::Session::off(11);
    std::uint32_t edits = 0;
    const Replay::StateHashes first = run(300, off, &edits);
    std::printf("1. 300 тиков: правок ландшафта %u\n   хеши подсистем: %s\n", edits, first.describe().c_str());
    EXPECT(edits == 2);

    // 2. Тот же сценарий ещё раз — хеши те же (детерминизм).
    Replay::Session again = Replay::Session::off(11);
    EXPECT(run(300, again) == first);
    std::printf("2. повторный прогон даёт те же хеши\n");

    // 3. Запись и повтор: сид + команды — в файл; повтор без ввода воспроизводит прогон до последнего бита.
    Replay::CommandRegistry registry;
    register_commands(registry); // схемы команд попадут в файл записи
    const std::string path = "headless_example.rec";
    {
        Replay::Session record = Replay::Session::record(11, path, &registry).value();
        const Replay::StateHashes recorded = run(300, record);
        EXPECT(record.finish(300, recorded).has_value());
    }
    auto replay = Replay::Session::from_args(std::vector<std::string>{"--replay", path}, 0);
    EXPECT(replay.has_value());
    const Replay::StateHashes replayed = run(300, *replay);
    const auto verdict = replay->finish(300, replayed);
    std::printf("3. повтор из файла: %s\n", Replay::describe(*verdict, replay->mode(), 300, 0).c_str());
    EXPECT(verdict->checked && verdict->match && replayed == first);
    std::remove(path.c_str());

    // 4. Расписание фаз тика. Новый модуль подключается своей фазой, не трогая SpellSim.
    Simulation sim;
    std::printf("4. фазы тика:");
    for (const std::string_view name : sim.schedule().names()) std::printf(" %.*s", static_cast<int>(name.size()), name.data());
    int machine_ticks = 0;
    sim.schedule().insert_after("movement", "machines", [&] { ++machine_ticks; });
    sim.tick({});
    sim.tick({});
    std::printf("\n   + «machines» после «movement»: выполнена %d раза; фаз теперь %zu, время тика %.3f мс\n", machine_ticks, sim.schedule().size(), sim.tick_ms());
    EXPECT(machine_ticks == 2);

    // 5. Состояние мира наружу только для чтения: изменить его можно лишь командами в тик — запись не обойти.
    const Terrain::SdfWorld& terrain = sim.terrain();
    std::printf("5. мир %d чанков, в очереди на меш %zu; мана в поле: %zu чанков\n", terrain.layout().chunk_count(), terrain.dirty_count(), sim.mana().allocated_chunks());
    std::printf("OK\n");
    return 0;
}
