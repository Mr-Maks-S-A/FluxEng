#include <Challenge/Play.hpp>

#include <doctest/doctest.h>

#include <set>

#include <cmath>
#include <filesystem>

using namespace Challenge;
using Math::Fixed;

namespace {

/// Бот-«ходок»: просто идёт к цели, заклинаний не знает. Контрольный игрок: уровни, требующие магии, он пройти не должен.
Solution walker(const Level& level) {
    Solution s;
    const Math::WorldPos goal = level.goal.point;
    s.policy = [goal](const SpellSim::Simulation& sim, std::vector<Replay::Command>& out) {
        if (sim.tick_number() % 10 != 0) return;
        const Math::WorldPos feet = sim.world().get<Character::Position>(sim.player())->value;
        const double dx = static_cast<double>(goal.x - feet.x), dz = static_cast<double>(goal.z - feet.z);
        const double len = std::sqrt(dx * dx + dz * dz);
        if (len > 1.0) out.push_back(SpellSim::move_command(Fixed::from_double(dx / len), Fixed::from_double(dz / len)));
    };
    return s;
}

} // namespace

TEST_CASE("кампания: у каждого уровня есть эталонное решение, и оно побеждает на три звезды") {
    REQUIRE(campaign().size() >= 4);
    for (const Level& level : campaign()) {
        CAPTURE(level.id);
        const Solution* solution = reference_solution(level.id);
        REQUIRE(solution != nullptr);
        const Outcome outcome = play(level, *solution);
        CHECK(outcome.status == Status::Won);
        CHECK(outcome.stars == 3);
        CHECK(outcome.loss == Loss::None);
    }
}

TEST_CASE("кампания: «ничего не делать» проигрывает везде, ходьба без магии — на всех уровнях, где нужна магия") {
    for (const Level& level : campaign()) {
        CAPTURE(level.id);
        const Outcome idle = play_idle(level);
        CHECK(idle.status == Status::Lost);
        CHECK(idle.loss == Loss::OutOfTime);

        const Outcome walked = play(level, walker(level));
        if (level.needs_spell) CHECK(walked.status == Status::Lost);
        else CHECK(walked.status == Status::Won);
    }
}

TEST_CASE("кампания: идентификаторы уникальны, у уровней есть описание, хеши настройки различаются и стабильны") {
    std::set<std::string> ids;
    std::set<std::uint64_t> hashes;
    for (const Level& level : campaign()) {
        CHECK(ids.insert(level.id).second);
        CHECK_FALSE(level.title.empty());
        CHECK_FALSE(level.brief.empty());
        CHECK(hashes.insert(level_hash(level)).second);
        CHECK(level_hash(level) == level_hash(level));
        CHECK(find_level(level.id) == &level);
    }
    CHECK(find_level("нет такого") == nullptr);
    CHECK(reference_solution("нет такого") == nullptr);
}

TEST_CASE("запись и повтор уровня: те же хеши и тот же исход, программа игрока (SetProgram) едет блобом в файле") {
    const Level& level = *find_level("mound"); // программа рисуется игроком: ссылка на блоб в потоке команд
    const Solution& solution = *reference_solution("mound");
    Replay::CommandRegistry registry;
    SpellSim::register_commands(registry);
    const std::string path = (std::filesystem::temp_directory_path() / "challenge_mound.rec").string();

    Outcome recorded;
    {
        auto session = Replay::Session::record(level.config.seed, path, &registry).value();
        recorded = play(level, solution, &session);
        INFO(recorded.status_line);
        REQUIRE(recorded.status == Status::Won);
        // Хеши последнего тика записываются в файл: берутся из симуляции в `play`, поэтому закрываем сессию там же, где знаем число тиков.
        REQUIRE(session.finish(recorded.ticks_run, recorded.hashes).has_value());
    }
    auto loaded = Replay::Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->blobs.size() == 1);

    auto replay = Replay::Session::replay(*loaded);
    const Outcome again = play(level, Solution{}, &replay); // сценарий берётся из записи
    CHECK(again.status == Status::Won);
    CHECK(again.hashes == recorded.hashes);
    CHECK(again.metrics.mana_spent == recorded.metrics.mana_spent);
    CHECK(again.stars == recorded.stars);
    std::filesystem::remove(path);
}

TEST_CASE("судья читает мир и не меняет его: прогон с судьёй и без дают одни хеши") {
    const Level& level = *find_level("mine");
    SpellSim::Simulation plain(level.config), watched(level.config);
    install_library(level, plain);
    install_library(level, watched);
    Referee referee(level);
    const Replay::Command cast = SpellSim::cast_command(0, Runes::ManaSource::Personal, {Fixed{}, Fixed::from_int(-1), Fixed{}});
    for (int t = 0; t < 120; ++t) {
        const std::span<const Replay::Command> cmds = t == 30 ? std::span(&cast, 1) : std::span<const Replay::Command>{};
        plain.tick(cmds);
        watched.tick(cmds);
        referee.observe(watched);
    }
    CHECK(plain.hashes() == watched.hashes());
}
