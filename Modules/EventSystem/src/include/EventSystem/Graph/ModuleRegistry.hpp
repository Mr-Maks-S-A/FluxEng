#pragma once
/**
 * @file ModuleRegistry.hpp
 * @brief Реестр модулей: кто какие события порождает и потребляет.
 */

#include <EventSystem/Core/Ids.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace EventSystem {

/**
 * @brief Декларация модуля: имя и его контракт по событиям.
 */
struct ModuleInfo {
    std::string name;              ///< Уникальное имя модуля.
    std::vector<EventId> produces; ///< События, которые модуль отправляет (в порядке объявления).
    std::vector<EventId> consumes; ///< События, которые модуль читает (в порядке объявления).
};

/**
 * @brief Хранит декларации модулей.
 *
 * Декларации нужны для контроля и визуализации, а не для доставки событий:
 * - EventBus::writer(ModuleId) / EventBus::reader(ModuleId) проверяют,
 *   что модуль объявил событие, и так ловят «незадокументированные» зависимости;
 * - EventGraph строит по декларациям граф зависимостей.
 *
 * Обычно используется через EventBus::declare_module(), а не напрямую.
 */
class ModuleRegistry {
public:
    /**
     * @brief Объявляет модуль.
     * @throws EventSystemError Если имя пустое или уже занято.
     */
    ModuleId declare(std::string_view name);

    /// @brief Добавляет событие в список порождаемых (повтор игнорируется).
    /// @throws EventSystemError Если модуль не объявлен.
    void add_production(ModuleId module, EventId event);

    /// @brief Добавляет событие в список потребляемых (повтор игнорируется).
    /// @throws EventSystemError Если модуль не объявлен.
    void add_consumption(ModuleId module, EventId event);

    /// @brief `true`, если модуль объявил, что порождает событие.
    [[nodiscard]] bool produces(ModuleId module, EventId event) const noexcept;
    /// @brief `true`, если модуль объявил, что потребляет событие.
    [[nodiscard]] bool consumes(ModuleId module, EventId event) const noexcept;

    /// @brief `true`, если идентификатор указывает на объявленный модуль.
    [[nodiscard]] bool contains(ModuleId module) const noexcept;

    /**
     * @brief Декларация модуля.
     * @throws EventSystemError Если модуль не объявлен.
     */
    [[nodiscard]] const ModuleInfo& info(ModuleId module) const;

    /// @brief Поиск модуля по имени.
    [[nodiscard]] std::optional<ModuleId> find(std::string_view name) const noexcept;

    /// @brief Количество модулей.
    [[nodiscard]] std::size_t size() const noexcept { return m_modules.size(); }

    /// @brief Все модули в порядке объявления; индекс в массиве равен ModuleId::index.
    [[nodiscard]] std::span<const ModuleInfo> all() const noexcept { return m_modules; }

private:
    ModuleInfo& checked(ModuleId module);

    std::vector<ModuleInfo> m_modules;
};

} // namespace EventSystem
