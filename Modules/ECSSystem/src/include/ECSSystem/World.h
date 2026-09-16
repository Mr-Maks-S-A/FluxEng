#pragma once

#include <ECSSystem/Core.h>
#include <ECSSystem/SparseSet.h>
#include <array>
#include <memory>
#include <vector>
#include <cassert>
#include <tuple>

namespace FluxECS {

    class World {
    public:
        World() {
            m_availableEntities.reserve(MAX_ENTITIES);
            // Заполняем в обратном порядке, чтобы pop_back() выдавал ID: 1, 2, 3...
            for (Entity entity = MAX_ENTITIES - 1; entity >= 1; --entity) {
                m_availableEntities.push_back(entity);
            }
        }

        Entity create_entity() {
            assert(!m_availableEntities.empty() && "Превышен лимит сущностей!");
            Entity id = m_availableEntities.back();
            m_availableEntities.pop_back();
            ++m_livingEntityCount;
            return id;
        }

        void destroy_entity(Entity entity) {
            assert(entity < MAX_ENTITIES && "Entity out of range");

            for (auto& storage : m_storages) {
                if (storage) {
                    storage->entityDestroyed(entity);
                }
            }

            m_entityMasks[entity].reset();
            m_availableEntities.push_back(entity); // Возвращаем ID на вершину стека
            --m_livingEntityCount;
        }

        // ==================== Компоненты ====================

        template <typename T, typename... Args>
        T& add_component(Entity entity, Args&&... args) {
            size_t typeID = getComponentTypeID<T>();
            auto* storage = get_or_create_storage<T>();
            
            T& comp = storage->emplace(entity, std::forward<Args>(args)...);
            m_entityMasks[entity].set(typeID);
            return comp;
        }

        template <typename T>
        void remove_component(Entity entity) {
            size_t typeID = getComponentTypeID<T>();
            auto* storage = get_storage<T>();
            if (storage && storage->remove(entity)) {
                m_entityMasks[entity].reset(typeID);
            }
        }

        template <typename T>
        T* get_component(Entity entity) {
            auto* storage = get_storage<T>();
            return storage ? storage->get(entity) : nullptr;
        }

        template <typename T>
        bool has_component(Entity entity) const {
            size_t typeID = getComponentTypeID<T>();
            return m_entityMasks[entity].test(typeID);
        }

        // ==================== Выполнение систем ====================

        template <typename... Components, typename F>
        void for_each(F&& func) {
            if constexpr (sizeof...(Components) == 0) return;

            ComponentMask targetMask;
            ((targetMask.set(getComponentTypeID<Components>())), ...);

            using FirstComponent = std::tuple_element_t<0, std::tuple<Components...>>;
            auto* firstStorage = get_storage<FirstComponent>();

            if (!firstStorage) return;

            const Entity* entities = firstStorage->entities();
            size_t count = firstStorage->size();

            for (size_t i = 0; i < count; ++i) {
                Entity e = entities[i];
                if ((m_entityMasks[e] & targetMask) == targetMask) {
                    func(e, *get_component<Components>(e)...);
                }
            }
        }

        size_t get_entity_count() const { return m_livingEntityCount; }

    private:
        std::vector<Entity> m_availableEntities;
        std::array<ComponentMask, MAX_ENTITIES> m_entityMasks{};
        std::array<std::unique_ptr<IComponentStorage>, MAX_COMPONENTS> m_storages{};
        size_t m_livingEntityCount = 0;

        template <typename T>
        ComponentStorage<T>* get_storage() {
            size_t typeID = getComponentTypeID<T>();
            return static_cast<ComponentStorage<T>*>(m_storages[typeID].get());
        }

        template <typename T>
        ComponentStorage<T>* get_or_create_storage() {
            size_t typeID = getComponentTypeID<T>();
            if (!m_storages[typeID]) {
                m_storages[typeID] = std::make_unique<ComponentStorage<T>>();
            }
            return static_cast<ComponentStorage<T>*>(m_storages[typeID].get());
        }
    };

} // namespace FluxECS