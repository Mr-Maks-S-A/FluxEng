#pragma once
/**
 * @file World.hpp
 * @brief Мир ECS: сущности, пулы компонентов и выборки.
 *
 * @code
 * ECS::World world;
 * ECS::Entity rabbit = world.create();
 * world.emplace<Position>(rabbit, 10.0f, 5.0f);
 * world.emplace<Velocity>(rabbit, 1.0f, 0.0f);
 *
 * world.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
 *     p.x += v.x;
 *     p.y += v.y;
 * });
 *
 * world.destroy(rabbit);          // все компоненты удалены, поколение слота выросло
 * world.valid(rabbit) == false;   // старая ссылка это «видит»
 * @endcode
 *
 * Созданный по умолчанию мир пуст и готов к работе (ZII).
 */

#include <ECSSystem/ComponentPool.hpp>
#include <ECSSystem/Entity.hpp>
#include <ECSSystem/View.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

namespace ECS {

namespace detail {

inline std::size_t next_component_id() noexcept {
    static std::atomic<std::size_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail

/**
 * @brief Номер типа компонента внутри процесса: 0, 1, 2… в порядке первого обращения.
 *
 * Используется только как индекс массива пулов. Номер **не** стабилен между запусками —
 * не сохраняйте его в файлы и не передавайте по сети (для этого нужно имя типа).
 */
template<typename T>
[[nodiscard]] std::size_t component_id() noexcept {
    static const std::size_t id = detail::next_component_id();
    return id;
}

/// @brief Мир ECS: реестр сущностей + пулы компонентов.
class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;
    ~World() = default;

    // ------------------------------------------------------------------ сущности

    /// @brief Создаёт пустую сущность.
    [[nodiscard]] Entity create() { return m_registry.create(); }

    /**
     * @brief Уничтожает сущность и все её компоненты.
     * @return `false`, если ссылка нулевая, устаревшая или сущность уже уничтожена.
     */
    bool destroy(Entity entity) {
        if (!m_registry.valid(entity)) {
            return false;
        }
        for (const auto& pool : m_pools) {
            if (pool) {
                pool->remove(entity);
            }
        }
        return m_registry.destroy(entity);
    }

    /// @brief `true`, если сущность жива и ссылка не устарела.
    [[nodiscard]] bool valid(Entity entity) const noexcept { return m_registry.valid(entity); }
    /// @brief Живых сущностей.
    [[nodiscard]] std::size_t alive() const noexcept { return m_registry.alive(); }
    /// @brief Реестр сущностей (только чтение).
    [[nodiscard]] const EntityRegistry& registry() const noexcept { return m_registry; }

    // ------------------------------------------------------------------ компоненты

    /**
     * @brief Добавляет компонент (или заменяет существующий).
     * @pre Сущность жива.
     * @return Ссылка на компонент; действительна до следующего добавления/удаления компонентов `T`.
     */
    template<typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args) {
        assert(valid(entity) && "World::emplace: entity is not alive");
        return pool<T>().emplace(entity, std::forward<Args>(args)...);
    }

    /// @brief Компонент или `nullptr` (нет компонента, сущность мертва или ссылка устарела).
    template<typename T>
    [[nodiscard]] T* get(Entity entity) noexcept {
        ComponentPool<T>* p = find_pool<T>();
        return p != nullptr ? p->get(entity) : nullptr;
    }
    /// @copydoc get
    template<typename T>
    [[nodiscard]] const T* get(Entity entity) const noexcept {
        const ComponentPool<T>* p = find_pool<T>();
        return p != nullptr ? p->get(entity) : nullptr;
    }

    /// @brief `true`, если у сущности есть компонент `T`.
    template<typename T>
    [[nodiscard]] bool has(Entity entity) const noexcept {
        const ComponentPool<T>* p = find_pool<T>();
        return p != nullptr && p->contains(entity);
    }

    /// @brief Удаляет компонент `T`. Возвращает `false`, если его не было.
    template<typename T>
    bool remove(Entity entity) {
        ComponentPool<T>* p = find_pool<T>();
        return p != nullptr && p->remove(entity);
    }

    /// @brief Пул компонентов `T` (создаётся при первом обращении).
    template<typename T>
    [[nodiscard]] ComponentPool<T>& pool() {
        const std::size_t id = component_id<T>();
        if (id >= m_pools.size()) {
            m_pools.resize(id + 1);
        }
        if (!m_pools[id]) {
            m_pools[id] = std::make_unique<ComponentPool<T>>();
        }
        return static_cast<ComponentPool<T>&>(*m_pools[id]);
    }

    /// @brief Пул компонентов `T` или `nullptr`, если его ещё нет.
    template<typename T>
    [[nodiscard]] ComponentPool<T>* find_pool() noexcept {
        const std::size_t id = component_id<T>();
        return id < m_pools.size() ? static_cast<ComponentPool<T>*>(m_pools[id].get()) : nullptr;
    }
    /// @copydoc find_pool
    template<typename T>
    [[nodiscard]] const ComponentPool<T>* find_pool() const noexcept {
        const std::size_t id = component_id<T>();
        return id < m_pools.size() ? static_cast<const ComponentPool<T>*>(m_pools[id].get()) : nullptr;
    }

    /// @brief Число компонентов `T`.
    template<typename T>
    [[nodiscard]] std::size_t count() const noexcept {
        const ComponentPool<T>* p = find_pool<T>();
        return p != nullptr ? p->size() : 0;
    }

    // ------------------------------------------------------------------ выборки

    /**
     * @brief Выборка сущностей со всеми компонентами `Ts...`. `const T` — только чтение.
     *
     * Пулы не создаются: если какого-то типа ещё нет, выборка просто пуста.
     */
    template<typename... Ts>
    [[nodiscard]] View<Ts...> view() noexcept {
        return View<Ts...>(find_pool<std::remove_const_t<Ts>>()...);
    }

    /// @brief Уничтожает все сущности и компоненты. Старые ссылки остаются невалидными.
    void clear() {
        for (const auto& p : m_pools) {
            if (p) p->clear();
        }
        m_registry.clear();
    }

private:
    EntityRegistry m_registry;
    std::vector<std::unique_ptr<IComponentPool>> m_pools; ///< Индекс — component_id<T>().
};

} // namespace ECS
