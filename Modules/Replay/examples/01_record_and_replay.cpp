/**
 * @example 01_record_and_replay.cpp
 * Запись и повтор: сид + команды каждого тика полностью задают прогон. Совпадение хешей на последнем тике
 * доказывает, что повтор идентичен. Файл самоописываем — смотреть и сравнивать записи можно без игры.
 *
 * «Игра» здесь — крошечная симуляция (счётчик и накопитель), чтобы был виден весь путь.
 */

#include <Math/Hash.hpp>
#include <Replay/Replay.hpp>

#include <cstdio>
#include <filesystem>

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

// Команды игры: тип и числа. Смысл полей игра описывает схемой (ниже) — она попадёт в файл.
enum : std::uint16_t { Add = 1, Reset = 2 };

/// Симуляция, пригодная для записи (концепт Replay::Simulatable): tick(команды), tick_number(), hashes().
struct Counter {
    std::uint32_t ticks = 0;
    std::int64_t sum = 0;
    void tick(std::span<const Replay::Command> commands) {
        for (const Replay::Command& c : commands) sum = c.type == Reset ? 0 : sum + c.x * (ticks + 1);
        ++ticks;
    }
    std::uint32_t tick_number() const { return ticks; }
    Replay::StateHashes hashes() const { // хеши названы по подсистемам: при расхождении видно, какая разошлась
        Math::Hasher total, ticks_hash;
        total.add_signed(sum);
        ticks_hash.add(ticks);
        Replay::StateHashes h;
        h.add("sum", total.value()).add("ticks", ticks_hash.value());
        return h;
    }
};
static_assert(Replay::Simulatable<Counter>);

int main() {
    // 1. Схемы команд: имена и типы полей. Игра регистрирует их один раз.
    Replay::CommandRegistry registry;
    registry.add({Add, "add", {Replay::CommandField{}, {"amount", Replay::CommandField::Kind::Int}, {}, {}}}).add({Reset, "reset", {}});

    const std::string file = (std::filesystem::temp_directory_path() / "example_replay.rec").string();

    // 2. Запись: Driver делает за игру всё — берёт живые команды, пишет их, тикает симуляцию, ведёт журнал тиков.
    Replay::StateHashes recorded;
    {
        Counter sim;
        // Запись идёт по ходу игры в файл-журнал с избыточностью (сброс на диск раз в секунду симуляции).
        Replay::Session session = Replay::Session::record(/*seed=*/7, file, &registry).value();
        Replay::FlightRecorder flight(8); // последние 8 тиков: при сбое их сбросит FLUX_ASSERT (install_assert_dump)
        Replay::Driver driver(sim, session, &flight);
        for (int t = 0; t < 100; ++t) {
            std::vector<Replay::Command> live; // «ввод игрока» этого тика
            if (t % 10 == 0) live.push_back({.type = Add, .x = t});
            if (t == 55) live.push_back({.type = Reset});
            EXPECT(driver.step(live));
        }
        const auto verdict = driver.finish(); // запись → файл
        EXPECT(verdict.has_value());
        recorded = verdict->actual;
        std::printf("1. записано: %s\n   журнал тиков хранит последние %zu\n", Replay::describe(*verdict, session.mode(), 100, session.recording().command_count()).c_str(),
                    flight.snapshot().size());
    }

    // 3. Повтор: ввод не нужен — команды берутся из файла; в конце хеши сверяются с записанными.
    {
        Counter sim;
        auto session = Replay::Session::from_args(std::vector<std::string>{"--replay", file}, 0);
        EXPECT(session.has_value());
        Replay::Driver driver(sim, *session);
        while (driver.step({})) {}
        const auto verdict = driver.finish();
        EXPECT(verdict.has_value() && verdict->match);
        std::printf("2. повтор: %s\n", Replay::describe(*verdict, session->mode(), 100, 0).c_str());
        EXPECT(verdict->actual == recorded);
    }

    // 4. Файл самоописываем: смотреть и сравнивать можно без знания игры (так работает ReplayTool).
    const auto recording = Replay::Recording::load(file);
    EXPECT(recording.has_value());
    std::printf("3. содержимое:\n%s", Replay::inspect(*recording, 4).c_str());

    Replay::Recording edited = *recording; // «другой» прогон: у него разошёлся хеш подсистемы «sum»
    edited.final_hashes = Replay::StateHashes{}.add("sum", recording->final_hashes.at("sum") ^ 1).add("ticks", recording->final_hashes.at("ticks"));
    const auto difference = Replay::diff(*recording, edited);
    EXPECT(difference.has_value() && difference->text.find("sum") != std::string::npos); // названа разошедшаяся подсистема
    std::printf("4. сравнение с испорченной копией: %s\n", difference->text.c_str());
    EXPECT(!Replay::diff(*recording, *recording).has_value());

    std::filesystem::remove(file);
    std::printf("OK\n");
    return 0;
}
