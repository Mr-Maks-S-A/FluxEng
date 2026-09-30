#include <ECSSystem/Entity.hpp>

#include <doctest/doctest.h>

#include <type_traits>
#include <unordered_set>

TEST_SUITE("ECSSystem.Entity") {

TEST_CASE("Entity{} — «нет сущности» (ZII)") {
    static_assert(std::is_trivially_copyable_v<ECS::Entity>);
    static_assert(sizeof(ECS::Entity) == 8);
    constexpr ECS::Entity none{};
    static_assert(none.is_null());
    static_assert(!none);
    static_assert(none == ECS::null_entity);

    ECS::EntityRegistry registry;
    CHECK_FALSE(registry.valid(none));
    CHECK(registry.alive() == 0);
}

TEST_CASE("первая сущность — слот 1, поколение 1; слот 0 не выдаётся") {
    ECS::EntityRegistry registry;
    const ECS::Entity a = registry.create();
    CHECK(a.index == 1);
    CHECK(a.generation == 1);
    CHECK(a);
    CHECK(registry.valid(a));
}

TEST_CASE("уничтожение: слот переиспользуется с новым поколением, старая ссылка невалидна") {
    ECS::EntityRegistry registry;
    const ECS::Entity a = registry.create();
    const ECS::Entity b = registry.create();
    CHECK(registry.destroy(a));
    CHECK_FALSE(registry.valid(a));
    CHECK(registry.valid(b));

    const ECS::Entity c = registry.create();
    CHECK(c.index == a.index);          // тот же слот
    CHECK(c.generation == a.generation + 1);
    CHECK_FALSE(registry.valid(a));     // старая ссылка по-прежнему мертва
    CHECK(registry.valid(c));
    CHECK(registry.alive() == 2);
}

TEST_CASE("повторное уничтожение и мусорные ссылки безопасны") {
    ECS::EntityRegistry registry;
    const ECS::Entity a = registry.create();
    CHECK(registry.destroy(a));
    CHECK_FALSE(registry.destroy(a));
    CHECK_FALSE(registry.destroy(ECS::Entity{}));
    CHECK_FALSE(registry.destroy(ECS::Entity{1000, 1}));
    CHECK(registry.alive() == 0);
}

TEST_CASE("each обходит только живых; clear сохраняет поколения") {
    ECS::EntityRegistry registry;
    const ECS::Entity a = registry.create();
    const ECS::Entity b = registry.create();
    const ECS::Entity c = registry.create();
    registry.destroy(b);

    std::unordered_set<ECS::Entity> seen;
    registry.each([&](ECS::Entity e) { seen.insert(e); });
    CHECK(seen.size() == 2);
    CHECK(seen.contains(a));
    CHECK(seen.contains(c));

    registry.clear();
    CHECK(registry.alive() == 0);
    CHECK_FALSE(registry.valid(a));
    const ECS::Entity d = registry.create();
    CHECK(d != a);
    CHECK(d != c);
}

}
