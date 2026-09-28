// Ожидаемая ошибка компиляции: events() у SoA-события.
#include "../TestEvents.hpp"

int main() {
    using namespace TestEvents;
    EventSystem::EventBus bus;
    auto reader = bus.reader<VoxelChangedEvent>();
    return static_cast<int>(reader.events().size());
}
