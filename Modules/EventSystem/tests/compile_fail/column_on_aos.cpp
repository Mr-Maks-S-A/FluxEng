// Ожидаемая ошибка компиляции: column() у AoS-события.
#include "../TestEvents.hpp"

int main() {
    using namespace TestEvents;
    EventSystem::EventBus bus;
    auto reader = bus.reader<CollisionEvent>();
    auto column = reader.column<&CollisionEvent::entity_a>();
    return static_cast<int>(column.size());
}
