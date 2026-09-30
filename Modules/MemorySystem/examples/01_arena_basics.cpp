/**
 * @example 01_arena_basics.cpp
 * Арена: нулевая память без конструкторов, откат к отметке, временная область.
 */

#include <MemorySystem/MemorySystem.hpp>

#include <cstdint>
#include <print>

namespace ms = MemorySystem;

// ZII-тип: все нули — корректное состояние «пустого уровня».
struct LevelHeader {
    std::uint32_t width;   // 0 — ещё не загружен
    std::uint32_t height;
    std::uint32_t entity_count;
};

int main() {
    // 256 МиБ адресов резервируются сразу, физическая память подтверждается по мере роста.
    ms::Arena level = ms::Arena::reserve(ms::MiB(256));

    LevelHeader* header = level.push<LevelHeader>();
    std::println("fresh header: {}x{}, {} entities (all zero, no constructor)", header->width, header->height,
                 header->entity_count);

    header->width = 64;
    header->height = 32;
    auto tiles = level.push_array<std::uint16_t>(header->width * header->height);
    std::println("tiles: {} values, tiles[100] = {}", tiles.size(), tiles[100]);

    // Временная память: всё, что выделено в области, освобождается и обнуляется на выходе.
    const std::size_t before = level.used();
    {
        ms::ArenaScope scratch(level);
        auto path = level.push_array<std::uint32_t>(4096);
        path[0] = 7;
        std::println("inside scope: used {} bytes", level.used());
    }
    std::println("after scope:  used {} bytes (was {})", level.used(), before);

    const ms::ArenaStats stats = level.stats();
    std::println("stats: used {} B, peak {} B, committed {} KiB of {} MiB reserved", stats.used, stats.peak,
                 stats.committed / 1024, stats.capacity / (1024 * 1024));

    level.reset(); // весь уровень освобождён одним вызовом
    std::println("after reset: used {} bytes", level.used());
}
