#include <Net/Net.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

using namespace Net;

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (const int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

/// Два узла на loopback; пустой результат — сокеты в этой среде недоступны (тест пропускается, а не падает).
struct Pair {
    std::unique_ptr<UdpTransport> a, b;
    bool ok = false;
    Pair() {
        auto ra = UdpTransport::open(0, {.bind_port = 0, .bind_address = "127.0.0.1"});
        auto rb = UdpTransport::open(1, {.bind_port = 0, .bind_address = "127.0.0.1"});
        if (!ra || !rb) return;
        a = std::move(*ra);
        b = std::move(*rb);
        ok = a->set_peer(1, "127.0.0.1", b->port()) && b->set_peer(0, "127.0.0.1", a->port());
    }
};

bool wait_for(Transport& t, Datagram& d, int ms = 500) {
    for (int i = 0; i < ms * 10; ++i) {
        if (t.receive(d)) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

} // namespace

TEST_CASE("UDP: датаграмма доходит по loopback с номером отправителя; пустая очередь — false") {
    Pair pair;
    if (!pair.ok) { MESSAGE("сокеты недоступны — тест пропущен"); return; }
    Datagram d;
    CHECK_FALSE(pair.b->receive(d));
    const auto payload = bytes_of({1, 2, 3, 250});
    pair.a->send(1, payload);
    REQUIRE(wait_for(*pair.b, d));
    CHECK(d.from == 0);
    CHECK(d.bytes == payload);
    CHECK(pair.a->stats().sent == 1);
    CHECK(pair.b->stats().received == 1);
}

TEST_CASE("UDP: от постороннего адреса и слишком большое отбрасывается и считается") {
    Pair pair;
    if (!pair.ok) { MESSAGE("сокеты недоступны — тест пропущен"); return; }
    auto stranger = UdpTransport::open(5, {.bind_port = 0, .bind_address = "127.0.0.1"});
    if (!stranger) { MESSAGE("сокеты недоступны — тест пропущен"); return; }
    REQUIRE((*stranger)->set_peer(1, "127.0.0.1", pair.b->port()));
    (*stranger)->send(1, bytes_of({9, 9, 9}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Datagram d;
    CHECK_FALSE(pair.b->receive(d));
    CHECK(pair.b->stats().unknown_sender == 1);

    pair.a->send(1, std::vector<std::byte>(max_packet_size + 1));
    CHECK(pair.a->stats().oversized == 1);
    CHECK(pair.a->stats().sent == 0);
}

TEST_CASE("UDP: открытие — ошибки текстом (чужой адрес, занятый порт)") {
    CHECK_FALSE(UdpTransport::open(0, {.bind_port = 0, .bind_address = "не адрес"}).has_value());
    Pair pair;
    if (!pair.ok) { MESSAGE("сокеты недоступны — тест пропущен"); return; }
    const auto busy = UdpTransport::open(2, {.bind_port = pair.a->port(), .bind_address = "127.0.0.1"});
    CHECK_FALSE(busy.has_value());
    CHECK(pair.a->set_peer(200, "127.0.0.1", 1) == false);
}

TEST_CASE("UDP + Lockstep: два пира по настоящему сокету идут в ногу и сходятся по хешам") {
    Pair pair;
    if (!pair.ok) { MESSAGE("сокеты недоступны — тест пропущен"); return; }
    Lockstep la(*pair.a, {.local = 0, .peers = 2, .seed = 7, .config_hash = 1});
    Lockstep lb(*pair.b, {.local = 1, .peers = 2, .seed = 7, .config_hash = 1});

    std::int64_t sum_a = 0, sum_b = 0;
    std::uint32_t ticks_a = 0, ticks_b = 0;
    const auto frame = [&](Lockstep& net, std::int64_t& sum, std::uint32_t& ticks, std::int32_t who) {
        net.pump();
        if (net.needs_input()) {
            const Replay::Command c{.type = 1, .x = who * 10 + static_cast<std::int32_t>(net.next_input_tick() % 7)};
            net.submit(std::span(&c, 1));
        }
        if (ticks < 120 && net.ready()) {
            for (const Replay::Command& c : net.advance()) sum += c.x * (c.player + 1);
            ++ticks;
            Replay::StateHashes h;
            h.add("sum", static_cast<std::uint64_t>(sum));
            net.report_hashes(h);
        }
    };
    for (int i = 0; i < 40000 && (ticks_a < 120 || ticks_b < 120); ++i) {
        frame(la, sum_a, ticks_a, 0);
        frame(lb, sum_b, ticks_b, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    CHECK(ticks_a == 120);
    CHECK(ticks_b == 120);
    CHECK(sum_a == sum_b);
    CHECK_FALSE(la.desync().has_value());
    CHECK_FALSE(lb.desync().has_value());
    CHECK(la.pumps_since_heard(1) < 1000); // слышали недавно
}

TEST_CASE("Lockstep: детектор молчания — пир, от которого ничего нет, всплывает в silent_peers") {
    SimulatedNetwork network(1);
    network.add_endpoint();
    network.add_endpoint();
    network.add_endpoint();
    Lockstep a(network.endpoint(0), {.local = 0, .peers = 3, .seed = 1});
    Lockstep b(network.endpoint(1), {.local = 1, .peers = 3, .seed = 1});
    // Пир 2 не создан как Lockstep: молчит. 0 и 1 переговариваются.
    for (int i = 0; i < 50; ++i) {
        a.pump();
        b.pump();
        network.advance_by(1000);
    }
    CHECK(a.pumps_since_heard(1) <= 1);
    CHECK(a.pumps_since_heard(2) == 50);
    CHECK(a.silent_peers(10) == std::vector<PeerId>{2});
    CHECK(a.silent_peers(100).empty());
    CHECK(a.pumps_since_heard(0) == 0); // себя — не считаем
}
