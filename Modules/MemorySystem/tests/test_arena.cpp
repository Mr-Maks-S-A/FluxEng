#include <MemorySystem/Arena.hpp>
#include <MemorySystem/ArenaResource.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace ms = MemorySystem;

namespace {

struct Particle {
    float x, y, vx, vy;
    std::uint32_t flags;
};

bool all_zero(const void* memory, std::size_t size) {
    const auto* bytes = static_cast<const std::byte*>(memory);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != std::byte{0}) return false;
    }
    return true;
}

} // namespace

TEST_SUITE("MemorySystem.Arena") {

TEST_CASE("пустая арена ничего не выделяет (ZII)") {
    ms::Arena arena;
    CHECK(arena.push(16) == nullptr);
    CHECK(arena.push<Particle>() == nullptr);
    CHECK(arena.push_array<int>(4).empty());
    CHECK(arena.used() == 0);
    arena.reset(); // безопасно
}

TEST_CASE("выделение выровнено и обнулено") {
    ms::Arena arena = ms::Arena::reserve(ms::MiB(1));
    REQUIRE(arena.capacity() >= ms::MiB(1));

    void* a = arena.push(3, 1);
    void* b = arena.push(64, 64);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
    CHECK(all_zero(b, 64));

    Particle* p = arena.push<Particle>();
    REQUIRE(p != nullptr);
    CHECK(p->x == 0.0f);
    CHECK(p->flags == 0u);

    std::span<double> values = arena.push_array<double>(100);
    REQUIRE(values.size() == 100);
    for (double v : values) CHECK(v == 0.0);
}

TEST_CASE("откат зануляет освобождённое: следующее выделение снова нулевое") {
    ms::Arena arena = ms::Arena::reserve(ms::MiB(1));
    const ms::ArenaMarker start = arena.mark();

    auto first = arena.push_array<std::uint32_t>(256);
    for (auto& v : first) v = 0xDEADBEEF;
    arena.pop_to(start);
    CHECK(arena.used() == 0);

    auto second = arena.push_array<std::uint32_t>(256);
    CHECK(second.data() == first.data());      // та же память
    CHECK(all_zero(second.data(), second.size_bytes()));
}

TEST_CASE("ArenaScope освобождает временную память") {
    ms::Arena arena = ms::Arena::reserve(ms::MiB(1));
    [[maybe_unused]] int* keep = arena.push<int>();
    const std::size_t before = arena.used();
    {
        ms::ArenaScope scope(arena);
        auto tmp = arena.push_array<int>(1000);
        tmp[500] = 42;
        CHECK(arena.used() > before);
    }
    CHECK(arena.used() == before);
    auto again = arena.push_array<int>(1000);
    CHECK(again[500] == 0);
}

TEST_CASE("виртуальная арена подтверждает память шагами и растёт до предела") {
    ms::Arena arena = ms::Arena::reserve(ms::MiB(4), ms::KiB(64));
    CHECK(arena.stats().committed == 0);

    REQUIRE(arena.push(ms::KiB(100), 16) != nullptr);
    CHECK(arena.stats().committed >= ms::KiB(100));
    CHECK(arena.stats().committed <= ms::KiB(192));

    REQUIRE(arena.push(ms::MiB(3), 16) != nullptr);
    CHECK(arena.push(ms::MiB(2), 16) == nullptr); // предел
    CHECK(arena.stats().peak >= ms::MiB(3));
}

TEST_CASE("shrink возвращает память выше позиции, после роста она снова нулевая") {
    ms::Arena arena = ms::Arena::reserve(ms::MiB(4), ms::KiB(64));
    auto big = arena.push_array<std::uint8_t>(ms::MiB(1));
    for (auto& b : big) b = 0xFF;
    arena.reset();
    arena.shrink();
    CHECK(arena.stats().committed == 0);
    auto again = arena.push_array<std::uint8_t>(ms::MiB(1));
    CHECK(all_zero(again.data(), again.size()));
}

TEST_CASE("арена над внешним буфером: буфер зануляется, рост невозможен") {
    std::array<std::byte, 256> buffer;
    buffer.fill(std::byte{0x77});
    ms::Arena arena = ms::Arena::over(buffer);
    CHECK(all_zero(buffer.data(), buffer.size()));
    CHECK(arena.push(200, 1) != nullptr);
    CHECK(arena.push(100, 1) == nullptr);
}

TEST_CASE("перемещение сохраняет адреса") {
    ms::Arena a = ms::Arena::reserve(ms::KiB(64));
    int* value = a.push<int>();
    *value = 5;
    ms::Arena b = std::move(a);
    CHECK(a.push<int>() == nullptr);
    CHECK(b.owns(value));
    CHECK(*value == 5);
}

TEST_CASE("DoubleArena: данные живут ровно один тик после создания") {
    auto frames = ms::DoubleArena::reserve(ms::MiB(1));
    int* tick0 = frames.current().push<int>();
    *tick0 = 10;

    frames.swap();                              // конец тика 0
    CHECK(frames.previous().owns(tick0));       // в тике 1 ещё читается
    CHECK(*tick0 == 10);
    [[maybe_unused]] int* tick1 = frames.current().push<int>();

    frames.swap();                              // конец тика 1: арена тика 0 очищена
    CHECK_FALSE(frames.current().owns(tick0));
    CHECK(frames.current().used() == 0);
    CHECK(*tick0 == 0);                         // и обнулена
}

TEST_CASE("ArenaResource: pmr-контейнеры внутри арены") {
    ms::Arena arena = ms::Arena::reserve(ms::MiB(1));
    ms::ArenaResource resource(arena);
    std::pmr::vector<int> numbers(&resource);
    for (int i = 0; i < 1000; ++i) numbers.push_back(i);
    CHECK(numbers[999] == 999);
    CHECK(arena.owns(numbers.data()));

    std::pmr::string name("a string long enough to leave the small buffer", &resource);
    CHECK(arena.owns(name.data()));
}

TEST_CASE("ArenaResource: исчерпание арены — std::bad_alloc") {
    std::array<std::byte, 128> buffer{};
    ms::Arena arena = ms::Arena::over(buffer);
    ms::ArenaResource resource(arena);
    std::pmr::vector<int> numbers(&resource);
    CHECK_THROWS_AS(numbers.resize(1000), std::bad_alloc);
}

}
