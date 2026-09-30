#include <ECSSystem/ComponentPool.hpp>

#include <doctest/doctest.h>

#include <string>

namespace {

struct Health {
    int value = 0;
};

// Считает живые экземпляры: проверяем, что пул не теряет и не дублирует деструкторы.
struct Tracked {
    static inline int live = 0;
    int payload = 0;
    Tracked() { ++live; }
    Tracked(int p) : payload(p) { ++live; }
    Tracked(const Tracked& o) : payload(o.payload) { ++live; }
    Tracked(Tracked&& o) noexcept : payload(o.payload) { ++live; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --live; }
};

} // namespace

TEST_SUITE("ECSSystem.ComponentPool") {

TEST_CASE("emplace / get / contains / remove") {
    ECS::ComponentPool<Health> pool;
    const ECS::Entity a{1, 1};
    const ECS::Entity b{2, 1};

    pool.emplace(a, 10);
    pool.emplace(b, 20);
    CHECK(pool.size() == 2);
    REQUIRE(pool.get(a) != nullptr);
    CHECK(pool.get(a)->value == 10);
    CHECK(pool.contains(b));
    CHECK_FALSE(pool.contains(ECS::Entity{3, 1}));

    CHECK(pool.remove(a));
    CHECK_FALSE(pool.remove(a));
    CHECK(pool.get(a) == nullptr);
    CHECK(pool.get(b)->value == 20); // b переставлен на место a
    CHECK(pool.size() == 1);
}

TEST_CASE("повторный emplace заменяет компонент, а не добавляет") {
    ECS::ComponentPool<Health> pool;
    const ECS::Entity a{1, 1};
    pool.emplace(a, 1);
    pool.emplace(a, 2);
    CHECK(pool.size() == 1);
    CHECK(pool.get(a)->value == 2);
}

TEST_CASE("устаревшее поколение не видит компонент нового владельца слота") {
    ECS::ComponentPool<Health> pool;
    const ECS::Entity old_ref{5, 1};
    const ECS::Entity new_ref{5, 2};
    pool.emplace(old_ref, 1);
    pool.emplace(new_ref, 2); // слот занят прошлым поколением: его компонент вытесняется
    CHECK(pool.size() == 1);
    CHECK(pool.get(old_ref) == nullptr);
    CHECK(pool.get(new_ref)->value == 2);
}

TEST_CASE("плотные массивы: entities() и components() параллельны") {
    ECS::ComponentPool<Health> pool;
    for (std::uint32_t i = 1; i <= 100; ++i) pool.emplace(ECS::Entity{i, 1}, static_cast<int>(i));
    for (std::uint32_t i = 1; i <= 100; i += 3) pool.remove(ECS::Entity{i, 1});
    REQUIRE(pool.entities().size() == pool.components().size());
    for (std::size_t k = 0; k < pool.size(); ++k) {
        CHECK(static_cast<std::uint32_t>(pool.components()[k].value) == pool.entities()[k].index);
    }
}

TEST_CASE("большие индексы: страницы создаются по требованию") {
    ECS::ComponentPool<Health> pool;
    const ECS::Entity far{1'000'000, 1};
    pool.emplace(far, 7);
    CHECK(pool.get(far)->value == 7);
    CHECK_FALSE(pool.contains(ECS::Entity{999'999, 1}));
}

TEST_CASE("деструкторы: не теряются и не дублируются") {
    Tracked::live = 0;
    {
        ECS::ComponentPool<Tracked> pool;
        for (std::uint32_t i = 1; i <= 50; ++i) pool.emplace(ECS::Entity{i, 1}, static_cast<int>(i));
        CHECK(Tracked::live == 50);
        for (std::uint32_t i = 1; i <= 50; i += 2) pool.remove(ECS::Entity{i, 1});
        CHECK(Tracked::live == 25);
        pool.clear();
        CHECK(Tracked::live == 0);
        pool.emplace(ECS::Entity{1, 2}, 1);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("компоненты с владением ресурсами") {
    ECS::ComponentPool<std::string> pool;
    pool.emplace(ECS::Entity{1, 1}, "rabbit");
    pool.emplace(ECS::Entity{2, 1}, "fox");
    pool.remove(ECS::Entity{1, 1});
    CHECK(*pool.get(ECS::Entity{2, 1}) == "fox");
}

}
