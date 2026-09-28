/**
 * @example 01_basic_stream.cpp
 * Минимальный цикл: объявить событие, зарегистрировать, отправить, прочитать в следующем тике.
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <print>
#include <string_view>

namespace es = EventSystem;

// Событие — обычная структура. Всё, что нужно шине, она объявляет о себе сама.
struct DamageEvent {
    std::uint32_t target = 0;
    float amount = 0.0f;

    static constexpr std::string_view event_name = "combat.damage";
    using fields = es::Fields<
        es::Field<"target", &DamageEvent::target>,
        es::Field<"amount", &DamageEvent::amount>>;
};

int main() {
    es::EventBus bus;
    bus.register_event<DamageEvent>();

    // Писатель и читатель получают один раз и хранят в системах.
    es::EventWriter<DamageEvent> damage_out = bus.writer<DamageEvent>();
    es::EventReader<DamageEvent> damage_in = bus.reader<DamageEvent>();

    for (int tick = 0; tick < 3; ++tick) {
        std::println("--- tick {} ---", bus.current_tick());

        // Читаем то, что отправили в прошлом тике.
        for (const DamageEvent& event : damage_in.events()) {
            std::println("  entity {} took {} damage", event.target, event.amount);
        }
        if (damage_in.empty()) {
            std::println("  no damage events");
        }

        // Отправляем новые: они станут видны в следующем тике.
        if (tick == 0) {
            damage_out.emit(DamageEvent{.target = 7, .amount = 15.0f});
            damage_out.emit(DamageEvent{.target = 9, .amount = 4.5f});
        }

        bus.advance_tick();
    }
}
