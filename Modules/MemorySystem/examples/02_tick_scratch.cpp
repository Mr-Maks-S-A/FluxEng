/**
 * @example 02_tick_scratch.cpp
 * DoubleArena: данные тика N читаются в тике N+1 и освобождаются сами — как события шины.
 */

#include <MemorySystem/MemorySystem.hpp>

#include <cstdint>
#include <print>
#include <span>

namespace ms = MemorySystem;

struct VisibleList {
    std::span<std::uint32_t> ids; // пусто, пока ничего не нашли (ZII)
};

int main() {
    auto ticks = ms::DoubleArena::reserve(ms::MiB(8));
    VisibleList last{};

    for (int tick = 0; tick < 4; ++tick) {
        // Читаем результат прошлого тика: он лежит в previous() и ещё жив.
        std::println("tick {}: previous list has {} ids{}", tick, last.ids.size(),
                     last.ids.empty() ? "" : " (still readable)");

        // Считаем новый список в current(): никаких free, никаких утечек.
        auto ids = ticks.current().push_array<std::uint32_t>(static_cast<std::size_t>(10 * (tick + 1)));
        for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<std::uint32_t>(i);
        last.ids = ids;

        ticks.swap(); // конец тика: арена позапрошлого тика очищена и обнулена
    }
}
