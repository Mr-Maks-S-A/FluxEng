/**
 * @example 05_lanes_policies_causes.cpp
 * Дорожки потоков (запись без мьютексов, SoA сохраняется), политики Coalesced/Scheduled и дерево причин.
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

namespace es = EventSystem;

// Массовое событие в SoA: каждое поле — своя колонка.
struct HitEvent {
    std::uint32_t target = 0;
    float damage = 0.0f;

    static constexpr std::string_view event_name = "combat.hit";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<es::Field<"target", &HitEvent::target>, es::Field<"damage", &HitEvent::damage>>;
};

// «Здоровье изменилось» — интерфейсу нужно только последнее значение на сущность за тик.
struct HealthChanged {
    std::uint32_t entity = 0;
    float health = 0.0f;

    static constexpr std::string_view event_name = "ui.health_changed";
    using fields = es::Fields<es::Field<"entity", &HealthChanged::entity>, es::Field<"health", &HealthChanged::health>>;
};

// Отложенное: яд срабатывает через несколько тиков.
struct PoisonTick {
    std::uint32_t entity = 0;

    static constexpr std::string_view event_name = "combat.poison";
    using fields = es::Fields<es::Field<"entity", &PoisonTick::entity>>;
};

int main() {
    es::EventBus bus;
    bus.register_event<HitEvent>(es::ChannelConfig{.trace = true});
    bus.register_event<HealthChanged>(es::ChannelConfig{
        .delivery = es::Delivery::Coalesced, .coalesce_field = es::coalesce_key<HealthChanged, &HealthChanged::entity>(), .trace = true});
    bus.register_event<PoisonTick>(es::ChannelConfig{.delivery = es::Delivery::Scheduled, .trace = true});

    auto hits = bus.writer<HitEvent>();
    auto hits_in = bus.reader<HitEvent>();
    auto health = bus.writer<HealthChanged>();
    auto health_in = bus.reader<HealthChanged>();
    auto poison = bus.writer<PoisonTick>();
    auto poison_in = bus.reader<PoisonTick>();

    // --- Тик 0: четыре потока проверяют попадания и пишут каждый в свою дорожку.
    constexpr std::size_t chunks = 4;
    auto lanes = hits.lanes(chunks); // главный поток, до работы
    std::vector<std::jthread> workers;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        workers.emplace_back([&lanes, chunk] {
            for (std::uint32_t i = 0; i < 3; ++i) lanes.emit(chunk, HitEvent{static_cast<std::uint32_t>(chunk), 10.0f + i});
        });
    }
    workers.clear(); // join
    bus.advance_tick(); // дорожки сливаются по порядку: результат тот же при любом числе потоков

    // --- Тик 1: реакции на попадания.
    std::println("tick {}: {} hits, targets column:", bus.current_tick(), hits_in.size());
    for (std::uint32_t t : hits_in.column<&HitEvent::target>()) std::print(" {}", t);
    std::println("");
    float hp[chunks] = {100.0f, 100.0f, 100.0f, 100.0f};
    for (std::size_t i = 0; i < hits_in.size(); ++i) {
        const HitEvent hit = hits_in.get(i);
        hp[hit.target] -= hit.damage;
        health.emit({hit.target, hp[hit.target]}, hits_in.ref(i)); // 12 событий → 4 после слияния
    }
    poison.emit_after({0}, 3, hits_in.ref(0)); // через 3 тика
    bus.advance_tick();

    std::println("tick {}: {} health updates (coalesced)", bus.current_tick(), health_in.size());
    for (const HealthChanged& h : health_in.events()) std::println("  entity {} -> {}", h.entity, h.health);

    while (poison_in.empty()) bus.advance_tick();
    std::println("tick {}: poison fired", bus.current_tick());

    // --- Откуда это взялось? Дерево следствий первого попадания (оно же — причина яда).
    // health от hit#0 сюда не попал: Coalesced оставил последнее значение (и его причину — hit#2).
    std::println("\ncause tree:\n{}", bus.trace_tree(poison_in.cause(0)));
    return 0;
}
