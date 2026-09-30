#pragma once
/**
 * @file ComponentPool.hpp
 * @brief Хранилище компонентов одного типа: sparse set со страничным разреженным массивом.
 *
 * ```
 * sparse (по слоту сущности, страницами по 4096):   [ 0 | 3 | 0 | 1 | 2 | 0 ... ]   0 = нет компонента
 *                                                          │       │   │
 * dense  (плотно, в порядке добавления):  entities   [ e3 | e4 | e1 ]
 *                                          components [ c3 | c4 | c1 ]   ← системы идут по этому массиву
 * ```
 *
 * - Добавление, поиск и удаление — O(1). Удаление переставляет последний элемент на место удалённого.
 * - Обход — по плотному массиву: без дыр, без проверок, дружелюбно к кэшу.
 * - Страницы разреженного массива создаются по требованию и **обнулены**: ноль значит «компонента нет»
 *   (ZII), поэтому новой странице не нужна инициализация.
 * - Предела числа сущностей нет (раньше был MAX_ENTITIES = 10000 и массивы фиксированного размера).
 */

#include <ECSSystem/Entity.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ECS {

/**
 * @brief Общий интерфейс пулов: нужен World, чтобы убрать все компоненты уничтоженной сущности.
 */
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    /// @brief Убирает компонент сущности, если он есть.
    virtual bool remove(Entity entity) = 0;
    /// @brief Число компонентов.
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    /// @brief Убирает все компоненты.
    virtual void clear() = 0;
};

/**
 * @brief Пул компонентов `T`.
 * @tparam T Тип компонента: перемещаемый. Для ZII удобно, когда `T{}` — осмысленное значение.
 */
template<typename T>
class ComponentPool final : public IComponentPool {
public:
    /// @brief Размер страницы разреженного массива (слотов).
    static constexpr std::size_t page_size = 4096;

    /**
     * @brief Добавляет компонент сущности или заменяет существующий.
     * @return Ссылка на компонент. Действительна до следующего добавления или удаления в этом пуле.
     */
    template<typename... Args>
    T& emplace(Entity entity, Args&&... args) {
        assert(!entity.is_null() && "ComponentPool::emplace: null entity");
        if (const std::uint32_t slot = sparse_slot(entity.index); slot != 0) {
            if (m_entities[slot - 1] == entity) {
                T& existing = m_components[slot - 1];
                existing = T{std::forward<Args>(args)...};
                return existing;
            }
            remove(m_entities[slot - 1]); // слот занят прошлым поколением: его компонент устарел
        }
        m_entities.push_back(entity);
        m_components.push_back(T{std::forward<Args>(args)...});
        sparse_slot(entity.index) = static_cast<std::uint32_t>(m_components.size()); // индекс + 1: ноль — «нет»
        return m_components.back();
    }

    /// @brief Компонент сущности или `nullptr`.
    [[nodiscard]] T* get(Entity entity) noexcept {
        const std::uint32_t dense = find(entity);
        return dense != 0 ? &m_components[dense - 1] : nullptr;
    }
    /// @copydoc get
    [[nodiscard]] const T* get(Entity entity) const noexcept {
        const std::uint32_t dense = find(entity);
        return dense != 0 ? &m_components[dense - 1] : nullptr;
    }

    /// @brief `true`, если у сущности (именно этого поколения) есть компонент.
    [[nodiscard]] bool contains(Entity entity) const noexcept { return find(entity) != 0; }

    /// @brief Убирает компонент (swap-and-pop). Возвращает `false`, если компонента не было.
    bool remove(Entity entity) override {
        const std::uint32_t dense = find(entity);
        if (dense == 0) {
            return false;
        }
        const std::size_t index = dense - 1;
        const std::size_t last = m_components.size() - 1;
        if (index != last) {
            m_components[index] = std::move(m_components[last]);
            m_entities[index] = m_entities[last];
            sparse_slot(m_entities[index].index) = static_cast<std::uint32_t>(index + 1);
        }
        m_components.pop_back();
        m_entities.pop_back();
        sparse_slot(entity.index) = 0;
        return true;
    }

    /// @brief Число компонентов.
    [[nodiscard]] std::size_t size() const noexcept override { return m_components.size(); }
    /// @brief `true`, если компонентов нет.
    [[nodiscard]] bool empty() const noexcept { return m_components.empty(); }

    /// @brief Сущности в плотном порядке (параллельно components()).
    [[nodiscard]] std::span<const Entity> entities() const noexcept { return m_entities; }
    /// @brief Компоненты в плотном порядке.
    [[nodiscard]] std::span<T> components() noexcept { return m_components; }
    /// @copydoc components
    [[nodiscard]] std::span<const T> components() const noexcept { return m_components; }

    /// @brief Убирает все компоненты (страницы остаются, обнуляются).
    void clear() override {
        for (const Entity e : m_entities) {
            sparse_slot(e.index) = 0;
        }
        m_entities.clear();
        m_components.clear();
    }

    /// @brief Резервирует место под `count` компонентов.
    void reserve(std::size_t count) {
        m_entities.reserve(count);
        m_components.reserve(count);
    }

private:
    /// @return Индекс в плотном массиве + 1, или 0.
    [[nodiscard]] std::uint32_t find(Entity entity) const noexcept {
        const std::size_t page = entity.index / page_size;
        if (page >= m_pages.size() || !m_pages[page]) {
            return 0;
        }
        const std::uint32_t dense = m_pages[page][entity.index % page_size];
        return dense != 0 && m_entities[dense - 1] == entity ? dense : 0;
    }

    std::uint32_t& sparse_slot(std::uint32_t index) {
        const std::size_t page = index / page_size;
        if (page >= m_pages.size()) {
            m_pages.resize(page + 1);
        }
        if (!m_pages[page]) {
            m_pages[page] = std::make_unique<std::uint32_t[]>(page_size); // обнулена: «компонентов нет»
        }
        return m_pages[page][index % page_size];
    }

    std::vector<std::unique_ptr<std::uint32_t[]>> m_pages;
    std::vector<Entity> m_entities;
    std::vector<T> m_components;
};

} // namespace ECS
