// Ожидаемая ошибка компиляции: событие с std::string не удовлетворяет концепту Event.
#include <EventSystem/EventSystem.hpp>

#include <string>

struct ChatMessageEvent {
    std::string text;

    static constexpr std::string_view event_name = "chat.message";
    using fields = EventSystem::Fields<EventSystem::Field<"text", &ChatMessageEvent::text>>;
};

int main() {
    EventSystem::EventBus bus;
    bus.register_event<ChatMessageEvent>();
}
