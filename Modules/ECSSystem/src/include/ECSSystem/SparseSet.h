#pragma once

#include <ECSSystem/Core.h>
#include <array>
#include <cassert>
#include <cstddef>
#include <utility>

namespace FluxECS {

    class IComponentStorage {
    public:
        virtual ~IComponentStorage() = default;
        virtual void entityDestroyed(Entity entity) = 0;
    };

    template<typename T, size_t MaxEntities = MAX_ENTITIES>
    class ComponentStorage : public IComponentStorage {
    public:
        template <typename... Args>
        T& emplace(Entity entity, Args&&... args) {
            assert(entity < MaxEntities && "Entity out of bounds");
            if (contains(entity)) {
                size_t idx = m_sparse[entity];
                m_dense[idx].~T();
                new (&m_dense[idx]) T(std::forward<Args>(args)...);
                return m_dense[idx];
            }

            size_t new_index = m_size;
            new (&m_dense[new_index]) T(std::forward<Args>(args)...);
            m_sparse[entity] = new_index;
            m_index_to_entity[new_index] = entity;
            ++m_size;

            return m_dense[new_index];
        }

        T* get(Entity entity) noexcept {
            if (!contains(entity)) return nullptr;
            return &m_dense[m_sparse[entity]];
        }

        bool contains(Entity entity) const noexcept {
            if (entity >= MaxEntities) return false;
            size_t index = m_sparse[entity];
            return index < m_size && m_index_to_entity[index] == entity;
        }

        bool remove(Entity entity) noexcept {
            if (!contains(entity)) return false;

            size_t index_to_remove = m_sparse[entity];
            size_t last_index = m_size - 1;
            
            m_dense[index_to_remove].~T();

            if (index_to_remove != last_index) {
                Entity last_entity = m_index_to_entity[last_index];
                new (&m_dense[index_to_remove]) T(std::move(m_dense[last_index]));
                m_dense[last_index].~T();
                
                m_sparse[last_entity] = index_to_remove;
                m_index_to_entity[index_to_remove] = last_entity;
            }

            m_sparse[entity] = sentinel;
            --m_size;
            return true;
        }

        void entityDestroyed(Entity entity) override {
            remove(entity);
        }

        size_t size() const noexcept { return m_size; }
        const Entity* entities() const noexcept { return m_index_to_entity.data(); }

        T* begin() noexcept { return m_dense.data(); }
        T* end() noexcept { return m_dense.data() + m_size; }

    private:
        static constexpr size_t sentinel = static_cast<size_t>(-1);

        alignas(64) std::array<T, MaxEntities> m_dense;
        alignas(64) std::array<size_t, MaxEntities> m_sparse;
        alignas(64) std::array<Entity, MaxEntities> m_index_to_entity;
        size_t m_size{0};

    public:
        ComponentStorage() {
            m_sparse.fill(sentinel);
        }

        ~ComponentStorage() override {
            for (size_t i = 0; i < m_size; ++i) {
                m_dense[i].~T();
            }
        }
    };

} // namespace FluxECS