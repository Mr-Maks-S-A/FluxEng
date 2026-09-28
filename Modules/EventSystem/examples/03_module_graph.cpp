/**
 * @example 03_module_graph.cpp
 * Модули объявляют контракт (что порождают и что читают), шина проверяет его
 * при выдаче писателей и читателей и строит граф зависимостей.
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <print>
#include <string_view>

namespace es = EventSystem;

// В реальном движке каждое событие лежит в публичном заголовке модуля-производителя,
// например Physics/Events.hpp. Потребители подключают только этот заголовок.

struct CollisionEvent {
    std::uint32_t entity_a = 0;
    std::uint32_t entity_b = 0;
    float impulse = 0.0f;

    static constexpr std::string_view event_name = "physics.collision";
    using fields = es::Fields<
        es::Field<"entity_a", &CollisionEvent::entity_a>,
        es::Field<"entity_b", &CollisionEvent::entity_b>,
        es::Field<"impulse", &CollisionEvent::impulse>>;
};

struct DamageEvent {
    std::uint32_t target = 0;
    float amount = 0.0f;

    static constexpr std::string_view event_name = "combat.damage";
    using fields = es::Fields<
        es::Field<"target", &DamageEvent::target>,
        es::Field<"amount", &DamageEvent::amount>>;
};

struct DeathEvent {
    std::uint32_t entity = 0;

    static constexpr std::string_view event_name = "health.death";
    using fields = es::Fields<es::Field<"entity", &DeathEvent::entity>>;
};

struct SpellCastEvent {
    std::uint32_t caster = 0;
    std::uint32_t spell = 0;

    static constexpr std::string_view event_name = "magic.spell_cast";
    using fields = es::Fields<
        es::Field<"caster", &SpellCastEvent::caster>,
        es::Field<"spell", &SpellCastEvent::spell>>;
};

int main() {
    es::EventBus bus;

    // Контракты модулей. Объявление заодно регистрирует каналы.
    const es::ModuleId physics = bus.declare_module("Physics").produces<CollisionEvent>();
    const es::ModuleId magic = bus.declare_module("Magic")
                                   .produces<SpellCastEvent>(es::ChannelConfig{.max_events_per_tick = 1024})
                                   .produces<DamageEvent>()
                                   .consumes<CollisionEvent>();
    const es::ModuleId combat = bus.declare_module("Combat").consumes<CollisionEvent>().produces<DamageEvent>();
    const es::ModuleId health = bus.declare_module("Health").consumes<DamageEvent>().produces<DeathEvent>();
    const es::ModuleId audio = bus.declare_module("Audio").consumes<CollisionEvent>().consumes<SpellCastEvent>();

    // Проверенный доступ: модуль получает только то, что объявил.
    auto collisions_out = bus.writer<CollisionEvent>(physics);
    auto collisions_in = bus.reader<CollisionEvent>(combat);
    auto damage_out = bus.writer<DamageEvent>(combat);
    auto damage_in = bus.reader<DamageEvent>(health);
    (void)magic;
    (void)audio;

    try {
        // Audio не объявлял, что пишет урон, — ошибка на старте, а не тихий баг.
        (void)bus.writer<DamageEvent>(audio);
    } catch (const es::EventSystemError& error) {
        std::println("contract violation caught: {}", error.what());
    }

    // Пара тиков работы.
    collisions_out.emit(CollisionEvent{.entity_a = 1, .entity_b = 2, .impulse = 12.0f});
    bus.advance_tick();
    for (const CollisionEvent& hit : collisions_in.events()) {
        damage_out.emit(DamageEvent{.target = hit.entity_b, .amount = hit.impulse * 2.0f});
    }
    bus.advance_tick();
    for (const DamageEvent& damage : damage_in.events()) {
        std::println("tick {}: entity {} takes {} damage", bus.current_tick(), damage.target, damage.amount);
    }

    // Граф зависимостей.
    const es::EventGraph graph = bus.build_graph();

    std::println("\n=== modules ===\n{}", graph.to_text());

    std::println("=== execution order ===");
    const es::ModuleOrder order = graph.module_order();
    for (const es::ModuleId module : order.order) {
        std::println("  {}", bus.modules().info(module).name);
    }

    std::println("\n=== warnings ===");
    for (const es::EventId id : graph.unconsumed_events()) {
        std::println("  nobody reads '{}'", graph.find_event(id)->name);
    }
    for (const es::EventId id : graph.unproduced_events()) {
        std::println("  nobody produces '{}'", graph.find_event(id)->name);
    }

    std::println("\n=== graphviz (dot -Tsvg graph.dot -o graph.svg) ===\n{}", graph.to_dot());
}
