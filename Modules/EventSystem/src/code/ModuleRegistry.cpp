#include <EventSystem/Core/Error.hpp>
#include <EventSystem/Graph/ModuleRegistry.hpp>

#include <algorithm>
#include <format>

namespace EventSystem {

namespace {

void add_unique(std::vector<EventId>& list, EventId event) {
    if (std::ranges::find(list, event) == list.end()) {
        list.push_back(event);
    }
}

} // namespace

ModuleId ModuleRegistry::declare(std::string_view name) {
    if (name.empty()) {
        throw EventSystemError("module name is empty");
    }
    if (find(name)) {
        throw EventSystemError(std::format("module '{}' is already declared", name));
    }
    m_modules.push_back(ModuleInfo{.name = std::string(name), .produces = {}, .consumes = {}});
    return ModuleId{static_cast<std::uint32_t>(m_modules.size() - 1)};
}

void ModuleRegistry::add_production(ModuleId module, EventId event) {
    add_unique(checked(module).produces, event);
}

void ModuleRegistry::add_consumption(ModuleId module, EventId event) {
    add_unique(checked(module).consumes, event);
}

bool ModuleRegistry::produces(ModuleId module, EventId event) const noexcept {
    return contains(module) && std::ranges::find(m_modules[module.index].produces, event) !=
                                   m_modules[module.index].produces.end();
}

bool ModuleRegistry::consumes(ModuleId module, EventId event) const noexcept {
    return contains(module) && std::ranges::find(m_modules[module.index].consumes, event) !=
                                   m_modules[module.index].consumes.end();
}

bool ModuleRegistry::contains(ModuleId module) const noexcept {
    return module.valid() && module.index < m_modules.size();
}

const ModuleInfo& ModuleRegistry::info(ModuleId module) const {
    if (!contains(module)) {
        throw EventSystemError("module is not declared");
    }
    return m_modules[module.index];
}

std::optional<ModuleId> ModuleRegistry::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < m_modules.size(); ++i) {
        if (m_modules[i].name == name) {
            return ModuleId{static_cast<std::uint32_t>(i)};
        }
    }
    return std::nullopt;
}

ModuleInfo& ModuleRegistry::checked(ModuleId module) {
    if (!contains(module)) {
        throw EventSystemError("module is not declared");
    }
    return m_modules[module.index];
}

} // namespace EventSystem
