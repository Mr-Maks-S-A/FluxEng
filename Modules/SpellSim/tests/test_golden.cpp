/**
 * Эталонные записи (golden): короткие `.rec`, закоммиченные в репозиторий, вместе с хешами подсистем на последнем тике.
 *
 * Зачем: детерминизм симуляции — главное свойство движка, а ломается он незаметно (другой компилятор, уровень
 * оптимизации, порядок обхода, неинициализированная память). Тест воспроизводит файлы, записанные раньше и, возможно,
 * другой сборкой, и требует тех же хешей по каждой подсистеме. Запуск в сборках gcc и clang, Release и Debug
 * ловит расхождения, пока они не вросли в код.
 *
 * Если поведение изменено **намеренно** (физика, цены рун, генерация мира), эталоны пересоздаются:
 *     FLUX_UPDATE_GOLDEN=1 ctest -R Golden        (или tools/update_golden.sh)
 * и новые файлы коммитятся вместе с изменением — в диффе видно, что поведение изменилось сознательно.
 */

#include <SpellSim/SpellSim.hpp>

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

using namespace SpellSim;
using Math::Fixed;
using Math::FVec3;

namespace {

struct Step {
    std::uint32_t tick;
    Replay::Command command;
};

struct Scenario {
    const char* name;      ///< Имя файла эталона.
    std::uint64_t seed;
    std::uint32_t ticks;
    std::vector<Step> steps;
};

const FVec3 down_forward = Math::quantize_direction(1.0, -1.0, 0.0);
const FVec3 down_left = Math::quantize_direction(-1.0, -0.8, 0.3);
const FVec3 straight_down = Math::quantize_direction(0.0, -1.0, 0.0);

/// A: прогулка с прыжками, оба заклинания обоими способами и «убегающий» цикл.
Scenario scenario_a() {
    return {"spellsim_a", 1, 1800,
            {{10, MoveCommand{Fixed::from_int(1), Fixed{}}.encode()},
             {60, CastCommand{0, Runes::ManaSource::Personal, down_forward}.encode()},
             {120, JumpCommand{}.encode()},
             {150, CastCommand{1, Runes::ManaSource::Ambient, down_forward}.encode()},
             {220, MoveCommand{Fixed{}, Fixed::from_int(1)}.encode()},
             {260, CastCommand{0, Runes::ManaSource::Ambient, down_forward}.encode()},
             {340, CastCommand{2, Runes::ManaSource::Personal, down_forward}.encode()},
             {460, MoveCommand{Fixed::from_ratio(-7, 10), Fixed::from_ratio(7, 10)}.encode()},
             {520, JumpCommand{}.encode()},
             {700, CastCommand{1, Runes::ManaSource::Personal, straight_down}.encode()},
             {900, MoveCommand{Fixed{}, Fixed{}}.encode()},
             {950, CastCommand{0, Runes::ManaSource::Personal, straight_down}.encode()},
             {1100, JumpCommand{}.encode()},
             {1400, MoveCommand{Fixed::from_int(1), Fixed::from_ratio(-1, 2)}.encode()}}};
}

/// B: другой сид, движение в сторону края мира, частые касты вперемешку (много правок ландшафта у границ чанков).
Scenario scenario_b() {
    Scenario s{"spellsim_b", 42, 1500, {}};
    s.steps.push_back({5, MoveCommand{Fixed::from_int(-1), Fixed::from_ratio(1, 3)}.encode()});
    for (std::uint32_t t = 30; t < 1400; t += 70) {
        s.steps.push_back({t, CastCommand{static_cast<int>((t / 70) % 2), (t / 70) % 3 == 0 ? Runes::ManaSource::Personal : Runes::ManaSource::Ambient,
                                          (t / 70) % 2 == 0 ? down_left : down_forward}.encode()});
        if (t % 140 == 30) s.steps.push_back({t + 20, JumpCommand{}.encode()});
    }
    s.steps.push_back({700, MoveCommand{Fixed::from_ratio(1, 2), Fixed::from_int(1)}.encode()});
    std::ranges::stable_sort(s.steps, {}, &Step::tick);
    return s;
}

void load_spells(Simulation& sim) {
    // Те же тексты, что в Sandbox/first_spell/spells: эталон не должен зависеть от того, где лежат файлы.
    REQUIRE(sim.programs().add_text("carve", "TARGET\nPUSH 2\nCARVE\nHALT\n").has_value());
    REQUIRE(sim.programs().add_text("raise", "TARGET\nPUSH 2\nRAISE\nHALT\n").has_value());
    REQUIRE(sim.programs().add_text("runaway", "loop:\n  PUSH 1\n  JMP_IF loop\n").has_value());
}

/// Прогоняет сценарий через Driver: запись, повтор или без записи — один и тот же путь.
Replay::StateHashes run(const Scenario& sc, Replay::Session& session) {
    Simulation sim(Config{.seed = session.seed()});
    load_spells(sim);
    Replay::Driver driver(sim, session);
    std::size_t next = 0;
    for (std::uint32_t t = 0; t < sc.ticks; ++t) {
        std::vector<Replay::Command> live;
        while (next < sc.steps.size() && sc.steps[next].tick == t) live.push_back(sc.steps[next++].command);
        if (!driver.step(live)) break;
    }
    return sim.hashes();
}

std::filesystem::path golden_path(const Scenario& sc) { return std::filesystem::path(SPELLSIM_GOLDEN_DIR) / (std::string(sc.name) + ".rec"); }

const char* how_to_update =
    "\nЕсли поведение изменено НАМЕРЕННО — пересоздайте эталоны: FLUX_UPDATE_GOLDEN=1 ctest -R Golden (или tools/update_golden.sh) и закоммитьте.\n"
    "Если нет — это потеря детерминизма: смотрите подсистемы выше и `ReplayTool diff`.";

void check_golden(const Scenario& sc) {
    const std::string path = golden_path(sc).string();
    Replay::CommandRegistry registry;
    register_commands(registry);

    if (std::getenv("FLUX_UPDATE_GOLDEN") != nullptr) { // пересоздание эталона
        auto session = Replay::Session::record(sc.seed, path, &registry).value();
        const Replay::StateHashes hashes = run(sc, session);
        REQUIRE(session.finish(sc.ticks, hashes).has_value());
        MESSAGE("эталон обновлён: " << path << " | " << hashes.describe());
        return;
    }

    REQUIRE_MESSAGE(std::filesystem::exists(path), "нет эталона " << path << how_to_update);
    auto loaded = Replay::Recording::load(path);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error());
    REQUIRE(loaded->complete);
    REQUIRE(loaded->final_hashes.count() == 6); // terrain, mana, characters, spells, rng, tick

    // 1. Повтор закоммиченной записи даёт те же хеши по каждой подсистеме.
    auto replay = Replay::Session::replay(*loaded);
    const Replay::StateHashes replayed = run(sc, replay);
    const auto verdict = replay.finish(sc.ticks, replayed).value();
    std::string differing;
    for (const std::string& n : verdict.differing) differing += ' ' + n;
    CHECK_MESSAGE(verdict.match, "эталон «" << sc.name << "»: разошлись подсистемы:" << differing << "\n  ожидалось: " << verdict.expected.describe()
                                           << "\n  получено:  " << verdict.actual.describe() << how_to_update);

    // 2. Сценарий в коде и закоммиченная запись — одно и то же: правка сценария без пересоздания эталона не пройдёт незамеченной.
    Replay::Session fresh = Replay::Session::record(sc.seed, {}, &registry).value();
    const Replay::StateHashes now = run(sc, fresh);
    REQUIRE(fresh.finish(sc.ticks, now).has_value());
    const auto difference = Replay::diff(*loaded, fresh.recording());
    CHECK_MESSAGE(!difference.has_value(), "эталон «" << sc.name << "» отличается от сценария: " << (difference ? difference->text : std::string{}) << how_to_update);
}

} // namespace

TEST_CASE("Golden: сценарий A (сид 1, прогулка, оба заклинания, убегающий цикл) воспроизводится побитово") { check_golden(scenario_a()); }

TEST_CASE("Golden: сценарий B (сид 42, движение к краю мира, частые касты) воспроизводится побитово") { check_golden(scenario_b()); }

TEST_CASE("Golden: эталоны чувствительны — другой сид или одна лишняя команда дают другие хеши") {
    if (std::getenv("FLUX_UPDATE_GOLDEN") != nullptr) return;
    const Scenario base = scenario_a();
    Replay::Session ref = Replay::Session::off(base.seed);
    const Replay::StateHashes reference = run(base, ref);

    Scenario other_seed = base;
    other_seed.seed = base.seed + 1;
    Replay::Session s1 = Replay::Session::off(other_seed.seed);
    CHECK(run(other_seed, s1).differing(reference).size() >= 2); // другой мир и все его следствия

    Scenario extra = base;
    extra.steps.push_back({1700, JumpCommand{}.encode()});
    std::ranges::stable_sort(extra.steps, {}, &Step::tick);
    Replay::Session s2 = Replay::Session::off(extra.seed);
    const auto changed = run(extra, s2).differing(reference);
    CHECK_FALSE(changed.empty()); // один прыжок на тике 1700 меняет хеш персонажа
    CHECK(std::ranges::find(changed, "characters") != changed.end());
}
