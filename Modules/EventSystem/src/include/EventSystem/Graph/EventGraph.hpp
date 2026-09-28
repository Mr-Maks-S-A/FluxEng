#pragma once
/**
 * @file EventGraph.hpp
 * @brief Граф зависимостей «модуль → событие → модуль» и его анализ.
 */

#include <EventSystem/Core/Ids.hpp>
#include <EventSystem/Graph/ModuleRegistry.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace EventSystem {

/**
 * @brief Узел-событие графа.
 */
struct EventNode {
    EventId id{};                    ///< Идентификатор события.
    std::string name;                ///< Имя события.
    std::vector<ModuleId> producers; ///< Модули, объявившие, что порождают событие.
    std::vector<ModuleId> consumers; ///< Модули, объявившие, что потребляют событие.
};

/**
 * @brief Ребро между модулями: `from` порождает событие `via`, которое потребляет `to`.
 */
struct ModuleEdge {
    ModuleId from{}; ///< Производитель.
    ModuleId to{};   ///< Потребитель.
    EventId via{};   ///< Событие, через которое они связаны.

    bool operator==(const ModuleEdge&) const = default;
};

/**
 * @brief Результат топологической сортировки модулей.
 */
struct ModuleOrder {
    /// @brief Модули в порядке «производитель раньше потребителя» (кроме модулей из циклов).
    std::vector<ModuleId> order;
    /// @brief Модули, входящие в циклы или зависящие от них.
    std::vector<ModuleId> cyclic;

    /// @brief `true`, если в графе есть циклы.
    [[nodiscard]] bool has_cycles() const noexcept { return !cyclic.empty(); }
};

/**
 * @brief Снимок графа зависимостей событий.
 *
 * Строится через EventBus::build_graph() и не зависит от шины после построения.
 * Используется для:
 * - поиска «висящих» событий (никто не порождает / никто не читает);
 * - определения порядка модулей и поиска циклов;
 * - визуализации: to_dot() для Graphviz и to_text() для консоли.
 *
 * @note Циклы допустимы: при политике Delivery::Stream ответ приходит в следующем тике,
 *       поэтому цикл не зависает, а растягивается по времени. Граф лишь делает его видимым.
 *       Ребро модуля в самого себя (модуль читает свои же события) в сортировке не учитывается.
 */
class EventGraph {
public:
    /**
     * @param modules Декларации модулей (индекс = ModuleId::index).
     * @param events  Все зарегистрированные события: пары (ID, имя) в порядке регистрации.
     */
    EventGraph(std::span<const ModuleInfo> modules, std::vector<std::pair<EventId, std::string>> events);

    /// @brief Модули графа.
    [[nodiscard]] std::span<const ModuleInfo> modules() const noexcept { return m_modules; }
    /// @brief События графа в порядке регистрации.
    [[nodiscard]] std::span<const EventNode> events() const noexcept { return m_events; }

    /// @brief Узел события или `nullptr`.
    [[nodiscard]] const EventNode* find_event(EventId id) const noexcept;

    /// @brief События, которые кто-то читает, но никто не объявил как порождаемые.
    [[nodiscard]] std::vector<EventId> unproduced_events() const;
    /// @brief События, которые кто-то порождает, но никто не читает.
    [[nodiscard]] std::vector<EventId> unconsumed_events() const;
    /// @brief Зарегистрированные события, не упомянутые ни одним модулем.
    [[nodiscard]] std::vector<EventId> orphan_events() const;

    /// @brief Все рёбра «производитель → потребитель» (без рёбер модуля в себя).
    [[nodiscard]] std::vector<ModuleEdge> module_edges() const;

    /**
     * @brief Топологическая сортировка модулей (алгоритм Кана).
     *
     * При равенстве модули идут в порядке объявления, поэтому результат детерминирован.
     */
    [[nodiscard]] ModuleOrder module_order() const;

    /// @brief Граф в формате Graphviz DOT (`dot -Tsvg graph.dot -o graph.svg`).
    [[nodiscard]] std::string to_dot() const;

    /// @brief Текстовое дерево «модуль → события → потребители» для консоли и логов.
    [[nodiscard]] std::string to_text() const;

private:
    [[nodiscard]] std::string_view event_name(EventId id) const noexcept;

    std::vector<ModuleInfo> m_modules;
    std::vector<EventNode> m_events;
};

} // namespace EventSystem
