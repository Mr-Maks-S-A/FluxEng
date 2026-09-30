/**
 * @example 04_pmr_containers.cpp
 * ArenaResource: стандартные pmr-контейнеры во временной арене.
 */

#include <MemorySystem/MemorySystem.hpp>

#include <memory_resource>
#include <print>
#include <string>
#include <vector>

namespace ms = MemorySystem;

int main() {
    ms::Arena scratch = ms::Arena::reserve(ms::MiB(16));
    {
        ms::ArenaResource resource(scratch);
        std::pmr::vector<std::pmr::string> names(&resource);
        names.reserve(3); // заранее: иначе старые буферы остаются в арене до reset
        names.emplace_back("rabbit with a name long enough to allocate");
        names.emplace_back("fox");
        names.emplace_back("grass");

        for (const auto& name : names) std::println("{}", name);
        std::println("arena used: {} bytes, first name inside arena: {}", scratch.used(),
                     scratch.owns(names.front().data()));
    } // контейнеры уничтожены: деструкторы вызваны, но память вернётся только при reset()
    scratch.reset();
    std::println("after reset: {} bytes", scratch.used());
}
