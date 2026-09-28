/**
 * @example 02_soa_voxels.cpp
 * Массовые события в SoA-раскладке: система читает только нужные колонки.
 * Здесь же — бюджет канала, защищающий тик от «взрыва» событий.
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <print>
#include <string_view>

namespace es = EventSystem;

struct VoxelChangedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t block = 0;

    static constexpr std::string_view event_name = "world.voxel_changed";
    static constexpr es::Layout layout = es::Layout::SoA; // колонка на поле
    using fields = es::Fields<
        es::Field<"x", &VoxelChangedEvent::x>,
        es::Field<"y", &VoxelChangedEvent::y>,
        es::Field<"z", &VoxelChangedEvent::z>,
        es::Field<"block", &VoxelChangedEvent::block>>;
};

constexpr std::uint32_t air = 0;
constexpr std::uint32_t stone = 1;

int main() {
    es::EventBus bus;
    bus.register_event<VoxelChangedEvent>(es::ChannelConfig{
        .reserve = 4096,              // без аллокаций в устойчивом режиме
        .max_events_per_tick = 10'000 // больше за тик — отбрасываем
    });

    auto voxels_out = bus.writer<VoxelChangedEvent>();
    auto voxels_in = bus.reader<VoxelChangedEvent>();

    // Тик 0: взрыв выкапывает куб 20x20x20 = 8000 вокселей и ещё 5000 «лишних».
    for (std::int32_t x = 0; x < 20; ++x) {
        for (std::int32_t y = 0; y < 20; ++y) {
            for (std::int32_t z = 0; z < 20; ++z) {
                voxels_out.emit(VoxelChangedEvent{.x = x, .y = y, .z = z, .block = air});
            }
        }
    }
    for (std::int32_t i = 0; i < 5000; ++i) {
        voxels_out.emit(VoxelChangedEvent{.x = i, .y = 0, .z = 0, .block = stone});
    }
    bus.advance_tick();

    // Тик 1: система освещения смотрит только на тип блока — читается одна колонка.
    std::size_t became_air = 0;
    for (const std::uint32_t block : voxels_in.column<&VoxelChangedEvent::block>()) {
        became_air += block == air ? 1 : 0;
    }

    // Система физики обломков читает координаты — три колонки, без типа блока.
    const auto xs = voxels_in.column<&VoxelChangedEvent::x>();
    const auto ys = voxels_in.column<&VoxelChangedEvent::y>();
    const auto zs = voxels_in.column<&VoxelChangedEvent::z>();
    std::int64_t checksum = 0;
    for (std::size_t i = 0; i < voxels_in.size(); ++i) {
        checksum += xs[i] + ys[i] + zs[i];
    }

    const es::IChannel& channel = *bus.find("world.voxel_changed");
    const es::ChannelStats stats = channel.stats();
    std::println("received {} voxel changes, {} became air, checksum {}", voxels_in.size(), became_air, checksum);
    std::println("dropped by budget: {}", stats.total_dropped);
    std::println("memory: {} bytes in {} columns", stats.allocated_bytes, channel.readable().column_count());
}
