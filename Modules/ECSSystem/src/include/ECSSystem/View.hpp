#pragma once
/**
 * @file View.hpp
 * @brief Выборка сущностей, у которых есть все компоненты `Ts...`.
 *
 * View не копирует данные и ничего не кэширует: это набор указателей на пулы.
 * Обход идёт по **самому маленькому** из пулов, остальные проверяются поиском O(1):
 * если у 100 000 сущностей есть Position и только у 50 — Burning, выборка
 * `view<Position, Burning>()` пройдёт 50 сущностей, а не 100 000.
 *
 * Обход идёт **с конца** плотного массива. Поэтому внутри each() можно безопасно
 * удалить или уничтожить **текущую** сущность: swap-and-pop переставит на её место
 * уже пройденную. Добавлять компоненты и удалять другие сущности во время обхода нельзя —
 * соберите их в список и примените после.
 */

#include <ECSSystem/ComponentPool.hpp>
#include <ECSSystem/Entity.hpp>

#include <cstddef>
#include <limits>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ECS {

/**
 * @brief Выборка сущностей с компонентами `Ts...`.
 * @tparam Ts Типы компонентов; `const T` — доступ только на чтение.
 */
template<typename... Ts>
class View {
    static_assert(sizeof...(Ts) > 0, "View needs at least one component type");

public:
    /// @brief Пулы в порядке `Ts...`; `nullptr`, если пула такого типа ещё нет (выборка пуста).
    explicit View(ComponentPool<std::remove_const_t<Ts>>*... pools) noexcept : m_pools(pools...) {}

    /**
     * @brief Вызывает `fn` для каждой подходящей сущности.
     *
     * Подходят обе формы: `fn(Entity, Ts&...)` и `fn(Ts&...)`.
     */
    template<typename Fn>
    void each(Fn&& fn) const {
        if (!all_pools_present()) {
            return;
        }
        // Ведущий пул выбирается один раз; дальше цикл специализирован под него на этапе компиляции.
        with_driver(smallest_pool(), [&]<std::size_t Driver>() { each_driven_by<Driver>(fn); });
    }

    /// @brief Верхняя оценка числа сущностей в выборке (размер наименьшего пула).
    [[nodiscard]] std::size_t size_hint() const noexcept {
        return all_pools_present() ? pool_size(smallest_pool()) : 0;
    }

    /// @brief `true`, если у сущности есть все компоненты выборки.
    [[nodiscard]] bool contains(Entity entity) const noexcept {
        return std::apply([&](auto*... pools) { return ((pools != nullptr && pools->contains(entity)) && ...); },
                          m_pools);
    }

private:
    template<typename Fn, typename... Cs>
    static void invoke(Fn& fn, Entity entity, Cs&... components) {
        if constexpr (std::is_invocable_v<Fn&, Entity, Ts&...>) {
            fn(entity, static_cast<Ts&>(components)...);
        } else {
            static_assert(std::is_invocable_v<Fn&, Ts&...>,
                          "View::each: callback must accept (Entity, Ts&...) or (Ts&...)");
            fn(static_cast<Ts&>(components)...);
        }
    }

    [[nodiscard]] bool all_pools_present() const noexcept {
        return std::apply([](auto*... pools) { return ((pools != nullptr) && ...); }, m_pools);
    }

    [[nodiscard]] std::size_t pool_size(std::size_t which) const noexcept {
        std::size_t result = 0;
        std::size_t index = 0;
        std::apply([&](auto*... pools) { ((index++ == which ? (result = pools->size(), 0) : 0), ...); }, m_pools);
        return result;
    }

    /// Вызывает `body.template operator()<D>()` для D == which (выбор ведущего пула во время выполнения).
    template<typename Body, std::size_t... Is>
    static void with_driver_impl(std::size_t which, Body& body, std::index_sequence<Is...>) {
        ((which == Is ? (body.template operator()<Is>(), 0) : 0), ...);
    }
    template<typename Body>
    static void with_driver(std::size_t which, Body&& body) {
        with_driver_impl(which, body, std::index_sequence_for<Ts...>{});
    }

    /// Компонент J-го типа для сущности из строки `row` ведущего пула D.
    template<std::size_t J, std::size_t D>
    [[nodiscard]] auto* component_for(Entity entity, std::size_t row) const noexcept {
        if constexpr (J == D) {
            return &std::get<D>(m_pools)->components()[row]; // ведущий пул: без поиска
        } else {
            return std::get<J>(m_pools)->get(entity);
        }
    }

    template<std::size_t D, typename Fn>
    void each_driven_by(Fn& fn) const {
        auto* driver = std::get<D>(m_pools);
        // С конца: удаление текущей сущности переставит на её место уже пройденную.
        for (std::size_t row = driver->size(); row-- > 0;) {
            if (row >= driver->size()) continue; // колбэк удалил несколько сущностей
            const Entity entity = driver->entities()[row];
            [&]<std::size_t... Js>(std::index_sequence<Js...>) {
                const auto components = std::make_tuple(component_for<Js, D>(entity, row)...);
                if (((std::get<Js>(components) != nullptr) && ...)) {
                    invoke(fn, entity, *std::get<Js>(components)...);
                }
            }(std::index_sequence_for<Ts...>{});
        }
    }

    [[nodiscard]] std::size_t smallest_pool() const noexcept {
        std::size_t best = 0;
        std::size_t best_size = std::numeric_limits<std::size_t>::max();
        std::size_t index = 0;
        std::apply(
            [&](auto*... pools) {
                ((pools->size() < best_size ? (best_size = pools->size(), best = index) : 0, ++index), ...);
            },
            m_pools);
        return best;
    }

    std::tuple<ComponentPool<std::remove_const_t<Ts>>*...> m_pools;
};

} // namespace ECS
