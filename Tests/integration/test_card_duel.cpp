/**
 * @file test_card_duel.cpp
 * @brief CardDuel (Sandbox) на модулях: правила + JobSystem (ИИ) + MemorySystem (арены, ZII) + EventSystem (журнал).
 *
 * Правила CardDuel — отдельная библиотека без графики (Sandbox/card_duel/Rules.*), поэтому партию ИИ против ИИ
 * можно сыграть прямо в тесте: проверить инварианты после каждого действия, детерминизм при любом числе
 * потоков и то, что журнал правил доходит до потребителя через шину по порядку.
 */

#include "Rules.hpp"

#include <EventSystem/EventSystem.hpp>
#include <JobSystem/JobSystem.hpp>
#include <MemorySystem/MemorySystem.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace cd = CardDuel;
namespace es = EventSystem;

namespace {

void check_invariants(const cd::Match& m) {
    std::set<std::uint32_t> uids;
    for (int p = 0; p < 2; ++p) {
        const cd::Player& player = m.players[static_cast<std::size_t>(p)];
        CHECK(player.hand.size() <= static_cast<std::size_t>(cd::max_hand));
        CHECK(player.board.size() <= static_cast<std::size_t>(cd::max_board));
        CHECK(player.mana <= player.max_mana);
        CHECK(player.max_mana <= cd::max_mana);
        CHECK(player.health <= player.max_health);
        for (const cd::HandCard& c : player.hand) {
            CHECK(c.uid >= 16);
            CHECK(uids.insert(c.uid).second); // uid уникален среди всех карт партии
        }
        for (const cd::Minion& minion : player.board) {
            CHECK(minion.health > 0); // погибшие убраны в том же действии
            CHECK(minion.health <= minion.max_health);
            CHECK(uids.insert(minion.uid).second);
        }
    }
}

struct MatchRun {
    std::uint64_t hash = 0;
    std::size_t actions = 0;
    std::size_t outcomes = 0;
    cd::Result result = cd::Result::None;
};

MatchRun play_match(std::uint64_t seed, unsigned threads, bool check = false) {
    JobSystem::Scheduler jobs(JobSystem::SchedulerConfig{.threads = threads});
    cd::OutcomeLog log;
    cd::Match match = cd::start_match(seed, cd::default_deck(0), cd::default_deck(1), &log);
    MatchRun run;
    for (int step = 0; step < 3000 && !match.over(); ++step) {
        const cd::AiDecision decision = cd::choose_action(match, jobs, cd::AiConfig{.rollouts = 12, .seed = match.active});
        if (check) {
            cd::ActionList legal;
            cd::legal_actions(match, legal);
            CHECK(std::all_of(legal.begin(), legal.end(), [&](const cd::Action& a) { return cd::is_legal(match, a); }));
            CHECK(std::find(legal.begin(), legal.end(), decision.action) != legal.end());
        }
        REQUIRE(cd::apply(match, decision.action, &log));
        if (check) check_invariants(match);
        ++run.actions;
    }
    run.hash = cd::hash(match);
    run.outcomes = log.size();
    run.result = match.result;
    return run;
}

cd::Minion minion(std::uint32_t uid, cd::CardId card, int attack, int health, std::uint8_t keywords = 0) {
    return cd::Minion{.uid = uid, .card = card, .attack = static_cast<std::int16_t>(attack), .health = static_cast<std::int16_t>(health),
                      .max_health = static_cast<std::int16_t>(health), .keywords = keywords};
}

/// Партия «посреди игры»: у обоих по 30 здоровья, ход игрока 0, 5 маны.
cd::Match board_state() {
    cd::Match m{};
    for (cd::Player& p : m.players) p.health = p.max_health = cd::hero_health;
    m.turn = 9;
    m.active = 0;
    m.next_uid = 100;
    m.players[0].mana = m.players[0].max_mana = 5;
    return m;
}

cd::Action attack(std::uint32_t source, std::uint32_t target) {
    return {.kind = static_cast<std::uint8_t>(cd::ActionKind::Attack), .player = 0, .source = source, .target = target};
}

} // namespace

TEST_SUITE("CardDuel + JobSystem + MemorySystem + EventSystem") {
    TEST_CASE("cpu: AI vs AI match keeps every rule invariant and finishes") {
        const MatchRun run = play_match(7, 3, true);
        CHECK(run.result != cd::Result::None);
        CHECK(run.actions > 10);
        CHECK(run.outcomes > run.actions);
    }

    TEST_CASE("cpu: the same seed gives the same match with 0, 1 and 7 job threads") {
        const MatchRun reference = play_match(2026, 0);
        for (const unsigned threads : {1u, 7u}) {
            CAPTURE(threads);
            const MatchRun run = play_match(2026, threads);
            CHECK(run.hash == reference.hash);
            CHECK(run.actions == reference.actions);
            CHECK(run.outcomes == reference.outcomes);
        }
        CHECK(play_match(2027, 0).hash != reference.hash); // и это действительно разные партии
    }

    TEST_CASE("cpu: commands and the rule journal travel through the event bus in order") {
        es::EventBus bus;
        const es::ModuleId ai = bus.declare_module("AI").produces<cd::Action>();
        const es::ModuleId rules = bus.declare_module("Rules").consumes<cd::Action>().produces<cd::Outcome>(
            es::ChannelConfig{.reserve = 64, .max_events_per_tick = 4096});
        const es::ModuleId scene = bus.declare_module("Scene").consumes<cd::Outcome>();
        auto commands_out = bus.writer<cd::Action>(ai);
        auto commands_in = bus.reader<cd::Action>(rules);
        auto journal_out = bus.writer<cd::Outcome>(rules);
        auto journal_in = bus.reader<cd::Outcome>(scene);

        JobSystem::Scheduler jobs(JobSystem::SchedulerConfig{.threads = 2});
        cd::OutcomeLog expected; // всё, что записали правила, — в порядке записи
        cd::OutcomeLog pending;
        cd::Match match = cd::start_match(5, cd::default_deck(0), cd::default_deck(1), &pending);
        std::size_t received = 0;
        bool mismatch = false;
        int ticks_after_end = 0; // журнал последнего действия доходит до Scene через тик
        for (int tick = 0; tick < 4000 && ticks_after_end < 2; ++tick) {
            // Scene: журнал прошлого тика.
            for (const cd::Outcome& o : journal_in.events()) {
                mismatch |= received >= expected.size() || o.kind != expected[received].kind || o.uid != expected[received].uid ||
                            o.value != expected[received].value;
                ++received;
            }
            // Rules: команды прошлого тика → журнал.
            for (const cd::Action& a : commands_in.events()) CHECK(cd::apply(match, a, &pending));
            for (const cd::Outcome& o : pending) {
                CHECK(journal_out.emit(o));
                expected.push_back(o);
            }
            pending.clear();
            // AI: одна команда за тик, если прошлая уже применена (как CommandGate в игре).
            if (!match.over() && commands_in.empty() && tick % 2 == 0) {
                commands_out.emit(cd::choose_action(match, jobs, cd::AiConfig{.rollouts = 8}).action);
            }
            bus.advance_tick();
            if (match.over()) ++ticks_after_end;
        }
        CHECK(match.over());
        CHECK_FALSE(mismatch);
        CHECK(received == expected.size());
        CHECK(bus.find("duel.outcome")->stats().total_dropped == 0);
    }

    TEST_CASE("cpu: Match is a ZII value: the zero match is not started and AI copies live in arenas") {
        const cd::Match zero{};
        CHECK_FALSE(zero.over());
        CHECK(zero.turn == 0);
        cd::ActionList legal;
        cd::legal_actions(zero, legal);
        CHECK(legal.empty());
        cd::Match copy = zero;
        CHECK_FALSE(cd::apply(copy, {.kind = static_cast<std::uint8_t>(cd::ActionKind::EndTurn)}));

        MemorySystem::Arena arena = MemorySystem::Arena::reserve(MemorySystem::MiB(4));
        cd::Match* in_arena = arena.push<cd::Match>();
        CHECK(cd::hash(*in_arena) == cd::hash(zero)); // нулевые байты арены — та же «пустая» партия
        *in_arena = cd::start_match(1, cd::default_deck(0), cd::default_deck(1));
        CHECK(in_arena->turn == 1);
    }

    TEST_CASE("cpu: combat rules — taunt, divine shield, charge, death resolution") {
        cd::Match m = board_state();
        m.players[0].board.push_back(minion(20, 12, 6, 7));                               // Огр 6/7
        m.players[1].board.push_back(minion(30, 3, 1, 4, cd::Keyword::taunt));            // Щитоносец 1/4, провокация
        m.players[1].board.push_back(minion(31, 6, 2, 3, cd::Keyword::divine_shield));    // Паладин 2/3, щит
        m.players[0].board[0].sleeping = false;

        CHECK_FALSE(cd::is_legal(m, attack(20, cd::hero_uid(1)))); // провокация закрывает героя
        CHECK_FALSE(cd::is_legal(m, attack(20, 31)));             // и других существ
        cd::OutcomeLog log;
        REQUIRE(cd::apply(m, attack(20, 30), &log));
        CHECK(m.players[1].board.size() == 1);                     // щитоносец погиб в том же действии
        CHECK(m.players[0].board[0].health == 6);                  // ответный удар 1
        CHECK(std::any_of(log.begin(), log.end(), [](const cd::Outcome& o) { return o.type() == cd::OutcomeKind::MinionDied && o.uid == 30; }));
        CHECK_FALSE(cd::is_legal(m, attack(20, 31)));              // уже атаковал в этом ходу

        // Следующий ход игрока 0: щит поглощает первый удар целиком.
        REQUIRE(cd::apply(m, {.kind = static_cast<std::uint8_t>(cd::ActionKind::EndTurn), .player = 0}));
        REQUIRE(cd::apply(m, {.kind = static_cast<std::uint8_t>(cd::ActionKind::EndTurn), .player = 1}));
        REQUIRE(cd::apply(m, attack(20, 31)));
        const cd::Minion* paladin = cd::find_minion(m, 31);
        REQUIRE(paladin != nullptr);
        CHECK(paladin->health == 3);
        CHECK_FALSE(paladin->has(cd::Keyword::divine_shield));

        // Рывок: сыгранное существо с рывком атакует сразу, без — спит.
        m.players[0].hand.push_back({200, 1}); // Гончая 1/1, рывок
        m.players[0].hand.push_back({201, 0}); // Новобранец 1/2
        REQUIRE(cd::apply(m, {.kind = static_cast<std::uint8_t>(cd::ActionKind::PlayCard), .player = 0, .source = 200}));
        REQUIRE(cd::apply(m, {.kind = static_cast<std::uint8_t>(cd::ActionKind::PlayCard), .player = 0, .source = 201}));
        CHECK(cd::is_legal(m, attack(200, cd::hero_uid(1)))); // рывок: бьёт сразу; паладин без провокации — герой открыт
        CHECK(cd::is_legal(m, attack(200, 31)));
        CHECK_FALSE(cd::is_legal(m, attack(201, 31)));                  // 201 спит
    }
}
