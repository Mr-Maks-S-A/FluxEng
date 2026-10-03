#include <Character/Character.hpp>

#include <algorithm>

namespace Character {

using Math::Fixed;
using Math::FVec3;
using Math::WorldPos;

ECS::Entity spawn(ECS::World& world, WorldPos feet, const ManaPool& mana, const Grimoire& grimoire) {
    const ECS::Entity e = world.create();
    world.emplace<Position>(e, Position{feet, feet});
    world.emplace<Velocity>(e);
    world.emplace<Collider>(e, Collider{{Fixed::from_ratio(2, 5), Fixed::from_ratio(9, 10), Fixed::from_ratio(7, 5)}, Fixed::from_ratio(2, 5), 3});
    world.emplace<ManaPool>(e, mana);
    world.emplace<Grimoire>(e, grimoire);
    world.emplace<Motor>(e);
    return e;
}

WorldPos eye(const ECS::World& world, ECS::Entity who) {
    const Position* p = world.get<Position>(who);
    return p ? WorldPos{p->value.x, p->value.y + Fixed::from_ratio(8, 5).raw, p->value.z} : WorldPos{}; // 1,6 м
}

namespace {

/// Выталкивает капсулу из породы. Возвращает true, если упёрлись в опору (ступаемый склон) снизу.
bool resolve(const Math::SdfField& terrain, const Collider& collider, const Config& config, WorldPos& pos, FVec3& velocity) {
    bool support = false;
    for (int iteration = 0; iteration < 4; ++iteration) {
        bool moved = false;
        for (int i = 0; i < collider.count; ++i) {
            const WorldPos centre{pos.x, pos.y + collider.heights[static_cast<std::size_t>(i)].raw, pos.z};
            const Fixed d = terrain.sample(centre) - collider.radius;
            if (d.raw >= 0) continue;
            FVec3 n = terrain.gradient(centre);
            if (n == FVec3{}) n = {Fixed{}, Fixed::from_int(1), Fixed{}};
            pos = Math::advance(pos, n, -d); // на глубину проникновения вдоль нормали
            const Fixed into = Math::dot(velocity, n);
            if (into.raw < 0) velocity = velocity - n * into; // гасим скорость «в породу»
            if (n.y >= config.walkable_slope) support = true;
            moved = true;
        }
        if (!moved) break;
    }
    return support;
}

} // namespace

void step(ECS::World& world, const Math::SdfField& terrain, const Config& config) {
    for (const ECS::Entity e : world.entities_of<Position>()) {
        Position& position = *world.get<Position>(e);
        Velocity& velocity = *world.get<Velocity>(e);
        const Collider& collider = *world.get<Collider>(e);
        ManaPool& mana = *world.get<ManaPool>(e);
        Motor& motor = *world.get<Motor>(e);

        mana.current = Math::min(mana.max, mana.current + mana.regen * config.tick);
        position.previous = position.value;

        // Управление: на земле скорость задаёт ввод, в воздухе — частично.
        const FVec3 target{motor.wish.x * config.walk_speed, Fixed{}, motor.wish.z * config.walk_speed};
        const Fixed control = motor.grounded ? Fixed::from_int(1) : config.air_control;
        velocity.value.x += (target.x - velocity.value.x) * control;
        velocity.value.z += (target.z - velocity.value.z) * control;
        if (motor.jump && motor.grounded) {
            velocity.value.y = config.jump_speed;
            motor.grounded = false;
        }
        motor.jump = false;
        if (!motor.grounded) {
            velocity.value = velocity.value + Math::gravity(position.value) * config.tick;
        } else if (velocity.value.y.raw < 0) {
            velocity.value.y = {};
        }
        velocity.value.y = Math::clamp(velocity.value.y, -config.terminal_speed, config.terminal_speed);

        const Fixed sub_dt = config.tick / Fixed::from_int(config.substeps);
        bool support = false;
        for (int s = 0; s < config.substeps; ++s) {
            position.value = Math::advance(position.value, velocity.value, sub_dt);
            support = resolve(terrain, collider, config, position.value, velocity.value) || support;
        }

        // Опора: контакт при столкновении или земля прямо под ногами (идём под уклон — не «отрываемся» от неё).
        // Во втором случае персонаж прилипает: опускается ровно на зазор, а не зависает над землёй.
        if (!support && velocity.value.y.raw <= 0) {
            const WorldPos feet_centre{position.value.x, position.value.y + collider.heights[0].raw, position.value.z};
            const Fixed gap = terrain.sample(feet_centre) - collider.radius;
            if (gap.raw >= 0 && gap <= config.snap_distance && terrain.gradient(feet_centre).y >= config.walkable_slope) {
                position.value.y -= gap.raw;
                support = true;
                resolve(terrain, collider, config, position.value, velocity.value);
            }
        }
        motor.grounded = support && velocity.value.y.raw <= 0;
    }
}

void hash_characters(const ECS::World& world, Math::Hasher& hasher) {
    for (const ECS::Entity e : world.entities_of<Position>()) {
        const Position& p = *world.get<Position>(e);
        const Velocity& v = *world.get<Velocity>(e);
        const ManaPool& m = *world.get<ManaPool>(e);
        hasher.add(e.index);
        hasher.add_signed(p.value.x), hasher.add_signed(p.value.y), hasher.add_signed(p.value.z);
        hasher.add_signed(v.value.x.raw), hasher.add_signed(v.value.y.raw), hasher.add_signed(v.value.z.raw);
        hasher.add_signed(m.current.raw());
        hasher.add(world.get<Motor>(e)->grounded ? 1 : 0);
    }
}

} // namespace Character
