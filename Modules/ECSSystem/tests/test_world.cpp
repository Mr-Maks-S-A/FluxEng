#include <ECSSystem/ECSSystem.hpp>

#include <doctest/doctest.h>

#include <map>
#include <random>
#include <set>
#include <vector>

namespace {

struct Position {
    float x = 0.0f, y = 0.0f;
};
struct Velocity {
    float x = 0.0f, y = 0.0f;
};
struct Burning {
    int ticks = 0;
};
struct Target {
    ECS::Entity entity; // Entity{} — «цели нет» (ZII)
};

} // namespace

TEST_SUITE("ECSSystem.World") {

TEST_CASE("созданный по умолчанию мир пуст и работает (ZII)") {
    ECS::World world;
    CHECK(world.alive() == 0);
    CHECK(world.get<Position>(ECS::Entity{}) == nullptr);
    CHECK(world.count<Position>() == 0);
    int calls = 0;
    world.view<Position>().each([&](Position&) { ++calls; });
    CHECK(calls == 0);
}

TEST_CASE("destroy удаляет все компоненты; старая ссылка ничего не находит") {
    ECS::World world;
    const ECS::Entity e = world.create();
    world.emplace<Position>(e, 1.0f, 2.0f);
    world.emplace<Velocity>(e);
    CHECK(world.has<Position>(e));

    CHECK(world.destroy(e));
    CHECK_FALSE(world.valid(e));
    CHECK(world.count<Position>() == 0);
    CHECK(world.count<Velocity>() == 0);

    const ECS::Entity reused = world.create();
    world.emplace<Position>(reused, 9.0f, 9.0f);
    CHECK(reused.index == e.index);
    CHECK(world.get<Position>(e) == nullptr); // устаревшая ссылка не видит нового владельца
    CHECK(world.get<Position>(reused)->x == 9.0f);
}

TEST_CASE("view: только сущности со всеми компонентами, обе формы колбэка") {
    ECS::World world;
    const ECS::Entity mover = world.create();
    world.emplace<Position>(mover);
    world.emplace<Velocity>(mover, 1.0f, 2.0f);
    const ECS::Entity rock = world.create();
    world.emplace<Position>(rock, 5.0f, 5.0f);

    int calls = 0;
    world.view<Position, const Velocity>().each([&](ECS::Entity e, Position& p, const Velocity& v) {
        CHECK(e == mover);
        p.x += v.x;
        p.y += v.y;
        ++calls;
    });
    CHECK(calls == 1);
    CHECK(world.get<Position>(mover)->y == 2.0f);
    CHECK(world.get<Position>(rock)->x == 5.0f);

    float sum = 0.0f;
    world.view<const Position>().each([&](const Position& p) { sum += p.x; });
    CHECK(sum == 6.0f);
}

TEST_CASE("view идёт по наименьшему пулу") {
    ECS::World world;
    for (int i = 0; i < 1000; ++i) {
        const ECS::Entity e = world.create();
        world.emplace<Position>(e);
        if (i % 100 == 0) world.emplace<Burning>(e, 3);
    }
    auto burning = world.view<Position, Burning>();
    CHECK(burning.size_hint() == 10);
    int calls = 0;
    burning.each([&](Position&, Burning&) { ++calls; });
    CHECK(calls == 10);
}

TEST_CASE("view: тип, которого ещё нет, — пустая выборка без создания пула") {
    ECS::World world;
    const ECS::Entity e = world.create();
    world.emplace<Position>(e);
    auto v = world.view<Position, Burning>();
    CHECK(v.size_hint() == 0);
    CHECK(world.find_pool<Burning>() == nullptr);
}

TEST_CASE("внутри each можно уничтожить текущую сущность") {
    ECS::World world;
    for (int i = 0; i < 100; ++i) {
        const ECS::Entity e = world.create();
        world.emplace<Burning>(e, i % 4);
        world.emplace<Position>(e);
    }
    int visited = 0;
    world.view<Burning, Position>().each([&](ECS::Entity e, Burning& b, Position&) {
        ++visited;
        if (b.ticks == 0) world.destroy(e);
    });
    CHECK(visited == 100); // никого не пропустили и не посетили дважды
    CHECK(world.count<Burning>() == 75);
    CHECK(world.alive() == 75);
}

TEST_CASE("ссылка на сущность в компоненте: Entity{} — «цели нет»") {
    ECS::World world;
    const ECS::Entity hunter = world.create();
    const ECS::Entity prey = world.create();
    Target& target = world.emplace<Target>(hunter);
    CHECK(target.entity.is_null());
    target.entity = prey;

    world.destroy(prey);
    const Target* t = world.get<Target>(hunter);
    CHECK_FALSE(world.valid(t->entity)); // охотник видит, что цель исчезла
}

TEST_CASE("рандомизированный прогон против эталонной модели") {
    ECS::World world;
    std::map<ECS::Entity, int> reference; // сущность → значение Burning (или -1, если компонента нет)
    std::vector<ECS::Entity> dead;
    std::mt19937 rng(12345);

    for (int step = 0; step < 50'000; ++step) {
        const auto action = static_cast<unsigned>(rng() % 6);
        if (action == 0 || reference.empty()) {
            const ECS::Entity e = world.create();
            reference[e] = -1;
        } else {
            auto it = reference.begin();
            std::advance(it, static_cast<long>(rng() % reference.size()));
            const ECS::Entity e = it->first;
            if (action == 1) {
                world.destroy(e);
                reference.erase(it);
                dead.push_back(e);
            } else if (action <= 3) {
                const int value = static_cast<int>(rng() % 1000);
                world.emplace<Burning>(e, value);
                it->second = value;
            } else {
                world.remove<Burning>(e);
                it->second = -1;
            }
        }
    }

    CHECK(world.alive() == reference.size());
    std::size_t with_component = 0;
    for (const auto& [e, value] : reference) {
        REQUIRE(world.valid(e));
        const Burning* b = world.get<Burning>(e);
        if (value < 0) {
            REQUIRE(b == nullptr);
        } else {
            REQUIRE(b != nullptr);
            REQUIRE(b->ticks == value);
            ++with_component;
        }
    }
    CHECK(world.count<Burning>() == with_component);
    for (const ECS::Entity e : dead) {
        REQUIRE_FALSE(world.valid(e));
        REQUIRE(world.get<Burning>(e) == nullptr);
    }

    std::set<ECS::Entity> seen;
    world.view<Burning>().each([&](ECS::Entity e, Burning&) { REQUIRE(seen.insert(e).second); });
    CHECK(seen.size() == with_component);
}

TEST_CASE("clear: всё удалено, старые ссылки невалидны") {
    ECS::World world;
    const ECS::Entity e = world.create();
    world.emplace<Position>(e);
    world.clear();
    CHECK(world.alive() == 0);
    CHECK(world.count<Position>() == 0);
    CHECK_FALSE(world.valid(e));
}

}
