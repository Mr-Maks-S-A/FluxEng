#include <MemorySystem/Pool.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace ms = MemorySystem;

namespace {

struct Bullet {
    float x, y, speed;
    std::uint32_t owner;
};

struct Tiny {
    std::uint8_t value; // меньше указателя: блок всё равно вмещает узел списка
};

} // namespace

TEST_SUITE("MemorySystem.Pool") {

TEST_CASE("пустой пул валиден (ZII)") {
    ms::Pool<Bullet> pool;
    CHECK(pool.allocate() == nullptr);
    pool.free(nullptr);
    CHECK(pool.live() == 0);
}

TEST_CASE("выдаёт нулевые объекты, повторно использует освобождённые") {
    auto pool = ms::Pool<Bullet>::reserve(16);
    Bullet* a = pool.allocate();
    REQUIRE(a != nullptr);
    CHECK(a->x == 0.0f);
    CHECK(a->owner == 0u);
    a->owner = 99;
    a->speed = 3.0f;
    CHECK(pool.live() == 1);

    pool.free(a);
    CHECK(pool.live() == 0);
    Bullet* b = pool.allocate();
    CHECK(b == a);                // LIFO: тот же блок
    CHECK(b->owner == 0u);        // и снова нулевой
    CHECK(b->speed == 0.0f);
}

TEST_CASE("блок вмещает узел списка даже для маленьких типов") {
    static_assert(ms::Pool<Tiny>::block_size >= sizeof(void*));
    auto pool = ms::Pool<Tiny>::reserve(4);
    Tiny* a = pool.allocate();
    Tiny* b = pool.allocate();
    CHECK(reinterpret_cast<std::uintptr_t>(b) - reinterpret_cast<std::uintptr_t>(a) == ms::Pool<Tiny>::block_size);
    pool.free(a);
    CHECK(pool.allocate()->value == 0);
}

TEST_CASE("предел ёмкости") {
    auto pool = ms::Pool<Bullet>::reserve(3);
    Bullet* items[3] = {pool.allocate(), pool.allocate(), pool.allocate()};
    for (Bullet* item : items) CHECK(item != nullptr);
    CHECK(pool.allocate() == nullptr);
    pool.free(items[1]);
    CHECK(pool.allocate() == items[1]);
}

TEST_CASE("случайная последовательность alloc/free: адреса уникальны, объекты нулевые") {
    auto pool = ms::Pool<Bullet>::reserve(1000);
    std::vector<Bullet*> live;
    std::mt19937 rng(7);
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || (rng() % 3 != 0 && live.size() < 1000)) {
            Bullet* b = pool.allocate();
            REQUIRE(b != nullptr);
            REQUIRE(b->owner == 0u);
            b->owner = static_cast<std::uint32_t>(step + 1);
            live.push_back(b);
        } else {
            const std::size_t i = rng() % live.size();
            pool.free(live[i]);
            live[i] = live.back();
            live.pop_back();
        }
    }
    CHECK(pool.live() == live.size());
    std::sort(live.begin(), live.end());
    CHECK(std::adjacent_find(live.begin(), live.end()) == live.end());
}

}
