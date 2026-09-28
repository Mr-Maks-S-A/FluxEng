// Ожидаемая ошибка компиляции: чтение колонки поля чужого события.
#include "../TestEvents.hpp"

int main() {
    using namespace TestEvents;
    EventSystem::EventBus bus;
    auto reader = bus.reader<VoxelChangedEvent>();
    auto column = reader.column<&DamageEvent::target>();
    return static_cast<int>(column.size());
}
