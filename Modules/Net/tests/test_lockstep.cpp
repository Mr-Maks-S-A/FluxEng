#include <Net/Lockstep.hpp>

#include <Math/Hash.hpp>
#include <Math/Rng.hpp>

#include <doctest/doctest.h>

#include <memory>

using namespace Net;

namespace {

enum : std::uint16_t { Add = 1, UseBlob = 2 };

/// Крошечная симуляция: сумма и номер тика. Команда UseBlob ссылается на блоб (хеш в x, y) и прибавляет его размер.
struct Toy {
    std::int64_t sum = 0;
    std::uint32_t ticks = 0;
    std::int64_t poison = 0; ///< Для теста расхождения: «недетерминизм» одного пира.

    void tick(std::span<const Replay::Command> commands, const Lockstep& net) {
        for (const Replay::Command& c : commands) {
            if (c.type == Add) sum += c.x * static_cast<std::int64_t>(ticks + 1);
            else if (c.type == UseBlob) {
                const std::uint64_t hash = static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y)) << 32);
                const auto* blob = net.blob(hash);
                REQUIRE(blob != nullptr); // сеть обязана доставить блоб раньше команды
                sum += static_cast<std::int64_t>(blob->size());
            }
        }
        sum += poison;
        ++ticks;
    }
    [[nodiscard]] Replay::StateHashes hashes() const {
        Math::Hasher a, b;
        a.add_signed(sum);
        b.add(ticks);
        Replay::StateHashes h;
        h.add("sum", a.value()).add("ticks", b.value());
        return h;
    }
};

std::optional<std::uint64_t> blob_ref(const Replay::Command& c) {
    if (c.type != UseBlob) return std::nullopt;
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y)) << 32);
}

struct Party {
    std::unique_ptr<LoopbackNetwork> net;
    std::vector<std::unique_ptr<Lockstep>> peers;
    std::vector<Toy> sims;
    std::vector<Math::Rng> inputs;
    std::vector<std::vector<Replay::Command>> injected; ///< Команды, которые пир добавит в ближайший свой ввод.
    std::uint32_t cap = ~0u;                            ///< Пир не исполняет тики после этого (но продолжает подавать ввод).

    Party(std::size_t n, const LinkConfig& link, std::uint64_t seed = 1, std::uint32_t delay = 3) : net(std::make_unique<LoopbackNetwork>(n, link, seed)), sims(n) {
        for (std::size_t i = 0; i < n; ++i) {
            LockstepConfig config{.local = static_cast<PeerId>(i), .peers = static_cast<std::uint8_t>(n), .seed = 9, .config_hash = 0xC0FFEE, .input_delay = delay};
            peers.push_back(std::make_unique<Lockstep>(net->endpoint(static_cast<PeerId>(i)), config));
            peers.back()->set_blob_reference(blob_ref);
            inputs.emplace_back(100 + i);
            injected.emplace_back();
        }
    }

    /// Один кадр всех пиров: pump, ввод, шаг (если готов), шаг сети. Ввод — случайные Add на ~1/3 тиков.
    void frame() {
        for (std::size_t i = 0; i < peers.size(); ++i) {
            Lockstep& p = *peers[i];
            p.pump();
            if (p.needs_input()) {
                std::vector<Replay::Command> mine;
                if (inputs[i].below(3) == 0) mine.push_back({.type = Add, .x = static_cast<std::int32_t>(inputs[i].below(100)) - 50});
                mine.insert(mine.end(), injected[i].begin(), injected[i].end());
                injected[i].clear();
                p.submit(mine);
            }
            if (sims[i].ticks < cap && p.ready()) {
                sims[i].tick(p.advance(), p);
                p.report_hashes(sims[i].hashes());
            }
        }
        net->step();
    }
    void run(int frames) { for (int i = 0; i < frames; ++i) frame(); }
    /// Все пиры доходят ровно до тика `target` (и стоят там): состояния сравнимы тик в тик. Плохой сети нужно больше кадров.
    void run_to(std::uint32_t target, int max_frames = 200000) {
        cap = target;
        for (int i = 0; i < max_frames && !all_at(target); ++i) frame();
        REQUIRE(all_at(target));
    }
    [[nodiscard]] bool all_at(std::uint32_t t) const {
        for (const Toy& s : sims) {
            if (s.ticks != t) return false;
        }
        return true;
    }
    [[nodiscard]] bool identical_at_same_tick() const {
        for (std::size_t i = 1; i < sims.size(); ++i) {
            if (sims[i].ticks != sims[0].ticks) return false;
        }
        return true;
    }
};

} // namespace

TEST_CASE("идеальная сеть: два пира идут в ногу, состояния и хеши совпадают на каждом тике") {
    Party party(2, {});
    party.run(300);
    CHECK(party.sims[0].ticks > 250);
    CHECK(party.sims[0].sum == party.sims[1].sum);
    CHECK(party.sims[0].ticks == party.sims[1].ticks);
    CHECK(party.sims[0].sum != 0);
    CHECK_FALSE(party.peers[0]->desync().has_value());
    CHECK_FALSE(party.peers[1]->desync().has_value());
}

TEST_CASE("плохая сеть: задержка, разброс, потери 20%, дубли и порча — на том же тике то же состояние, что и без неё") {
    Party ideal(2, {}, 1);
    Party bad(2, {.latency_steps = 3, .jitter_steps = 4, .loss_permille = 200, .duplicate_permille = 100, .corrupt_permille = 50}, 7);
    ideal.run_to(500);
    bad.run_to(500); // плохой сети нужно больше кадров, но состояние на тике 500 то же: ввод каждого пира зависит только от номера тика
    CHECK(bad.net->stats().lost > 0);
    CHECK(bad.net->stats().corrupted > 0);
    CHECK(bad.peers[0]->stats().bad_packets + bad.peers[1]->stats().bad_packets > 0); // порча поймана контрольной суммой
    CHECK_FALSE(bad.peers[0]->desync().has_value());
    CHECK_FALSE(bad.peers[1]->desync().has_value());
    CHECK(bad.sims[0].sum == bad.sims[1].sum);
    CHECK(bad.sims[0].sum == ideal.sims[0].sum);
    CHECK(ideal.sims[0].sum != 0);
}

TEST_CASE("итог не зависит от качества сети: три пира, три разные сети — одно состояние на тике 400") {
    Party a(3, {}, 1), b(3, {.latency_steps = 2, .jitter_steps = 3, .loss_permille = 150}, 5), c(3, {.latency_steps = 6, .loss_permille = 300, .duplicate_permille = 200}, 9);
    a.run_to(400);
    b.run_to(400);
    c.run_to(400);
    CHECK(a.sims[0].sum == a.sims[1].sum);
    CHECK(a.sims[1].sum == a.sims[2].sum);
    CHECK(b.sims[0].sum == a.sims[0].sum);
    CHECK(c.sims[2].sum == a.sims[0].sum);
}

TEST_CASE("три пира: ввод каждого учитывается, никто не отстаёт и не расходится") {
    Party party(3, {.latency_steps = 1, .jitter_steps = 2, .loss_permille = 50}, 3);
    party.run_to(300);
    CHECK(party.sims[0].sum == party.sims[1].sum);
    CHECK(party.sims[1].sum == party.sims[2].sum);
    for (auto& p : party.peers) CHECK_FALSE(p->desync().has_value());
}

TEST_CASE("блоб: большая программа доходит по плохой сети, команда ждёт её и исполняется у всех одинаково") {
    Party party(2, {.latency_steps = 2, .jitter_steps = 2, .loss_permille = 250}, 11);
    std::vector<std::byte> program(2600); // три куска
    for (std::size_t i = 0; i < program.size(); ++i) program[i] = static_cast<std::byte>(i * 31);
    party.run(10);
    const std::uint64_t hash = party.peers[0]->submit_blob(program);
    // Команда со ссылкой уходит сразу, не дожидаясь блоба: исполнение у обоих должно подождать.
    party.injected[0].push_back({.type = UseBlob, .x = static_cast<std::int32_t>(static_cast<std::uint32_t>(hash)), .y = static_cast<std::int32_t>(static_cast<std::uint32_t>(hash >> 32))});
    party.run_to(400);
    REQUIRE(party.peers[1]->blob(hash) != nullptr);
    CHECK(*party.peers[1]->blob(hash) == program);
    CHECK(party.sims[0].sum == party.sims[1].sum);
    CHECK(party.peers[1]->stats().blobs_received == 1);
    // Блоб действительно был исполнен (иначе тест ничего не проверял): без него сумма случайных Add другая.
    Party without(2, {.latency_steps = 2, .jitter_steps = 2, .loss_permille = 250}, 11);
    without.run(10);
    without.run_to(400);
    CHECK(party.sims[0].sum - without.sims[0].sum == 2600);
}

TEST_CASE("несовпадение настройки или сида — отказ при знакомстве, шаг не делается") {
    LoopbackNetwork net(2);
    Lockstep a(net.endpoint(0), {.local = 0, .peers = 2, .seed = 1, .config_hash = 111});
    Lockstep b(net.endpoint(1), {.local = 1, .peers = 2, .seed = 1, .config_hash = 222});
    for (int i = 0; i < 50; ++i) {
        a.pump(); b.pump();
        if (a.needs_input()) a.submit({});
        if (b.needs_input()) b.submit({});
        net.step();
    }
    CHECK_FALSE(a.ready());
    CHECK_FALSE(b.ready());
    CHECK(a.failure().find("настройка") != std::string::npos);
    CHECK_FALSE(a.connected());

    LoopbackNetwork net2(2);
    Lockstep c(net2.endpoint(0), {.local = 0, .peers = 2, .seed = 1});
    Lockstep d(net2.endpoint(1), {.local = 1, .peers = 2, .seed = 2});
    for (int i = 0; i < 20; ++i) { c.pump(); d.pump(); net2.step(); }
    CHECK(c.failure().find("сид") != std::string::npos);
}

TEST_CASE("расхождение: недетерминированный пир обнаружен на первом тике, называется подсистема") {
    Party party(2, {.latency_steps = 1}, 4);
    party.run(60);
    party.sims[1].poison = 1; // с этого момента второй пир считает «по-своему»
    party.run(200);
    REQUIRE(party.peers[0]->desync().has_value());
    CHECK(party.peers[0]->desync()->subsystems == std::vector<std::string>{"sum"}); // ticks совпадают — разошлась только сумма
    CHECK(party.peers[0]->desync()->peer == 1);
    CHECK(party.peers[0]->desync()->tick >= 55);
    CHECK(party.peers[0]->desync()->tick < 80);
}

TEST_CASE("обрыв связи: игра не падает и не идёт, после восстановления догоняет") {
    Party party(2, {.latency_steps = 1}, 8);
    party.run(100);
    const std::uint32_t before = party.sims[0].ticks;
    party.net->set_link({.loss_permille = 1000});
    party.run(200);
    CHECK(party.sims[0].ticks <= before + 10); // уже подтверждённая задержка ввода позволяет пройти пару тиков, не больше
    CHECK(party.peers[0]->stats().stalled_pumps > 100);
    party.net->set_link({.latency_steps = 1});
    party.run(300);
    CHECK(party.sims[0].ticks > before + 200);
    CHECK(party.sims[0].sum == party.sims[1].sum);
}

TEST_CASE("один игрок: сеть не нужна, шаги идут сразу") {
    LoopbackNetwork net(1);
    Lockstep solo(net.endpoint(0), {.local = 0, .peers = 1});
    for (int i = 0; i < 20; ++i) {
        solo.pump();
        if (solo.needs_input()) solo.submit({});
        REQUIRE(solo.ready());
        (void)solo.advance();
    }
    CHECK(solo.tick() == 20);
}

TEST_CASE("чужие и испорченные датаграммы не ломают протокол") {
    LoopbackNetwork net(2);
    Lockstep a(net.endpoint(0), {.local = 0, .peers = 2, .seed = 1});
    const std::byte junk[] = {std::byte{1}, std::byte{2}};
    net.endpoint(1).send(0, junk);
    // «подделка отправителя»: пакет от пира 1 с полем from = 0
    const auto spoof = encode(Packet{0, BlobAck{1}});
    net.endpoint(1).send(0, spoof);
    net.step();
    a.pump();
    CHECK(a.stats().bad_packets == 2);
}
