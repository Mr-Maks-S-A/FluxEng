#include <NetSystem/NetSystem.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace NetSystem;

namespace {

std::vector<std::byte> payload(std::uint32_t value) {
    ByteWriter w;
    w.write(value);
    return w.take();
}

std::uint32_t value_of(const Packet& p) {
    ByteReader r(p.data);
    return r.read<std::uint32_t>();
}

/// Отправляет N пакетов, прокручивает время и возвращает порядок доставки (значения) и времена.
struct Trace {
    std::vector<std::uint32_t> values;
    std::vector<std::uint64_t> times;
};

Trace run(SimulatedNetwork& net, ITransport& a, ITransport& b, int count, std::uint64_t until_us) {
    for (int i = 0; i < count; ++i) a.send(1, payload(static_cast<std::uint32_t>(i)));
    Trace trace;
    Packet p;
    for (std::uint64_t t = 0; t <= until_us; t += 500) {
        net.advance_to(t);
        while (b.receive(p)) {
            trace.values.push_back(value_of(p));
            trace.times.push_back(t);
        }
    }
    return trace;
}

} // namespace

TEST_SUITE("NetSystem.SimulatedNetwork") {

TEST_CASE("PeerId выдаются подряд, пакет несёт адрес отправителя") {
    SimulatedNetwork net(1, {.latency_us = 1000});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    CHECK(a.local_id() == 0);
    CHECK(b.local_id() == 1);
    a.send(1, payload(7));
    net.advance_to(1000);
    Packet p;
    REQUIRE(b.receive(p));
    CHECK(p.from == 0);
    CHECK(value_of(p) == 7);
    CHECK_FALSE(b.receive(p));
}

TEST_CASE("задержка точная: раньше срока пакета нет, в срок — есть") {
    SimulatedNetwork net(1, {.latency_us = 40'000});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    a.send(1, payload(1));
    Packet p;
    net.advance_to(39'999);
    CHECK_FALSE(b.receive(p));
    CHECK(net.in_flight() == 1);
    net.advance_to(40'000);
    CHECK(b.receive(p));
    CHECK(net.in_flight() == 0);
}

TEST_CASE("без джиттера порядок сохраняется") {
    SimulatedNetwork net(5, {.latency_us = 10'000});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    const Trace t = run(net, a, b, 100, 20'000);
    REQUIRE(t.values.size() == 100);
    for (std::uint32_t i = 0; i < 100; ++i) CHECK(t.values[i] == i);
}

TEST_CASE("джиттер переставляет пакеты, но укладывается в границы") {
    SimulatedNetwork net(5, {.latency_us = 10'000, .jitter_us = 8'000});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    const Trace t = run(net, a, b, 200, 30'000);
    REQUIRE(t.values.size() == 200);
    bool reordered = false;
    for (std::size_t i = 1; i < t.values.size(); ++i) reordered |= t.values[i] < t.values[i - 1];
    CHECK(reordered);
    for (const std::uint64_t at : t.times) {
        CHECK(at >= 10'000);
        CHECK(at <= 18'500); // 10 мс + 8 мс джиттера + шаг опроса 0,5 мс
    }
}

TEST_CASE("потери: доля близка к заданной") {
    SimulatedNetwork net(9, {.latency_us = 1000, .loss = 0.2});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    const Trace t = run(net, a, b, 10'000, 3000);
    const double lost = 1.0 - static_cast<double>(t.values.size()) / 10'000.0;
    CHECK(lost > 0.17);
    CHECK(lost < 0.23);
    CHECK(net.stats().dropped + net.stats().delivered == net.stats().sent);
}

TEST_CASE("дубликаты: лишние копии приходят, статистика сходится") {
    SimulatedNetwork net(3, {.latency_us = 1000, .jitter_us = 500, .duplicate = 0.25});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    const Trace t = run(net, a, b, 4000, 4000);
    CHECK(t.values.size() == 4000 + net.stats().duplicated);
    CHECK(net.stats().duplicated > 800);
    CHECK(net.stats().duplicated < 1200);
}

TEST_CASE("воспроизводимость: тот же seed — та же доставка, другой seed — другая") {
    const auto trace_of = [](std::uint64_t seed) {
        SimulatedNetwork net(seed, {.latency_us = 5000, .jitter_us = 4000, .loss = 0.1, .duplicate = 0.1});
        ITransport& a = net.add_endpoint();
        ITransport& b = net.add_endpoint();
        return run(net, a, b, 500, 15'000);
    };
    const Trace one = trace_of(77), same = trace_of(77), other = trace_of(78);
    CHECK(one.values == same.values);
    CHECK(one.times == same.times);
    CHECK(one.values != other.values);
}

TEST_CASE("связь настраивается по направлениям") {
    SimulatedNetwork net(1, {.latency_us = 1000});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    net.set_link(0, 1, {.latency_us = 50'000});
    a.send(1, payload(1));
    b.send(0, payload(2));
    net.advance_to(1000);
    Packet p;
    CHECK(a.receive(p));       // обратное направление быстрое
    CHECK_FALSE(b.receive(p)); // прямое — 50 мс
    net.advance_to(50'000);
    CHECK(b.receive(p));
}

TEST_CASE("слишком большие датаграммы и неизвестный адресат отбрасываются и считаются") {
    SimulatedNetwork net(1, {.latency_us = 0});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    a.send(1, std::vector<std::byte>(kMaxPacketBytes + 1));
    a.send(9, payload(1));
    a.send(1, std::vector<std::byte>(kMaxPacketBytes)); // ровно в предел — проходит
    net.advance_to(0);
    Packet p;
    CHECK(b.receive(p));
    CHECK_FALSE(b.receive(p));
    CHECK(net.stats().oversized == 1);
    CHECK(net.stats().dropped == 1);
}

TEST_CASE("время не идёт назад; пакет самому себе тоже ходит с задержкой") {
    SimulatedNetwork net(1, {.latency_us = 100});
    ITransport& a = net.add_endpoint();
    net.advance_to(500);
    net.advance_to(100);
    CHECK(net.now_us() == 500);
    a.send(0, payload(3));
    net.advance_to(599);
    Packet p;
    CHECK_FALSE(a.receive(p));
    net.advance_to(600);
    CHECK(a.receive(p));
}

} // TEST_SUITE
