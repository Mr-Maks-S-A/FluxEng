#pragma once
/**
 * @file Entity.hpp
 * @brief Сущность как ссылка с поколением и реестр, который их выдаёт.
 *
 * Сущность — это не объект, а **ссылка**: номер слота + поколение слота.
 * Когда сущность уничтожается, поколение слота растёт, и все старые ссылки на неё
 * перестают быть валидными, даже если слот уже достался новой сущности.
 *
 * ```
 * create()  → {index 5, generation 1}
 * destroy({5,1})                       слот 5: generation 1 → 2, слот свободен
 * create()  → {index 5, generation 2}  тот же слот, новое поколение
 * valid({5,1}) == false                старая ссылка «видит», что сущность умерла
 * ```
 *
 * **ZII.** `Entity{}` = `{0, 0}` — это «нет сущности». Слот 0 никогда не выдаётся,
 * а поколение живой сущности всегда ≥ 1, поэтому нулевая ссылка не совпадёт ни с одной живой.
 */

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ECS {

/**
 * @brief Ссылка на сущность: слот + поколение. 8 байт, trivially copyable.
 *
 * Нулевая сущность (`Entity{}`) — «нет сущности»; её можно хранить в компонентах
 * как «цель не выбрана» без отдельного флага.
 */
struct Entity {
    std::uint32_t index = 0;      ///< Слот в реестре (0 — зарезервирован под «нет сущности»).
    std::uint32_t generation = 0; ///< Поколение слота на момент создания (у живых ≥ 1).

    /// @brief `true` для `Entity{}`.
    [[nodiscard]] constexpr bool is_null() const noexcept { return index == 0; }
    /// @brief `true`, если ссылка не нулевая (жива ли сущность — спрашивайте World::valid()).
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return index != 0; }

    friend constexpr bool operator==(Entity, Entity) noexcept = default;
    friend constexpr auto operator<=>(Entity, Entity) noexcept = default;
};

/// @brief Нулевая сущность — «нет сущности».
inline constexpr Entity null_entity{};

/**
 * @brief Выдаёт и освобождает слоты сущностей, следит за поколениями.
 *
 * Свободные слоты переиспользуются в порядке LIFO: последний освобождённый выдаётся первым
 * (горячий в кэше). Созданный по умолчанию реестр пуст и валиден (ZII).
 */
class EntityRegistry {
public:
    /// @brief Создаёт сущность (переиспользует свободный слот, если есть).
    [[nodiscard]] Entity create() {
        if (m_generations.empty()) {
            m_generations.push_back(0); // слот 0 — «нет сущности», никогда не выдаётся
        }
        std::uint32_t index = 0;
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            index = static_cast<std::uint32_t>(m_generations.size());
            m_generations.push_back(0);
        }
        ++m_generations[index]; // новое поколение: старые ссылки на этот слот больше не совпадут
        m_alive.resize(m_generations.size(), false);
        m_alive[index] = true;
        ++m_alive_count;
        return {index, m_generations[index]};
    }

    /**
     * @brief Уничтожает сущность. Нулевые, чужие и уже мёртвые ссылки игнорируются.
     * @return `true`, если сущность была жива.
     */
    bool destroy(Entity entity) {
        if (!valid(entity)) {
            return false;
        }
        m_alive[entity.index] = false;
        m_free.push_back(entity.index);
        --m_alive_count;
        return true;
    }

    /// @brief `true`, если сущность жива и ссылка не устарела.
    [[nodiscard]] bool valid(Entity entity) const noexcept {
        return entity.index != 0 && entity.index < m_generations.size() && m_alive[entity.index] &&
               m_generations[entity.index] == entity.generation;
    }

    /// @brief Живых сущностей.
    [[nodiscard]] std::size_t alive() const noexcept { return m_alive_count; }
    /// @brief Слотов всего (живых и свободных), без нулевого.
    [[nodiscard]] std::size_t slots() const noexcept { return m_generations.empty() ? 0 : m_generations.size() - 1; }

    /// @brief Вызывает `fn(Entity)` для каждой живой сущности (в порядке слотов).
    template<typename Fn>
    void each(Fn&& fn) const {
        for (std::size_t i = 1; i < m_generations.size(); ++i) {
            if (m_alive[i]) {
                fn(Entity{static_cast<std::uint32_t>(i), m_generations[i]});
            }
        }
    }

    /// @brief Уничтожает все сущности; поколения сохраняются, чтобы старые ссылки остались невалидными.
    void clear() {
        m_free.clear();
        for (std::size_t i = m_generations.size(); i-- > 1;) {
            m_alive[i] = false;
            m_free.push_back(static_cast<std::uint32_t>(i));
        }
        m_alive_count = 0;
    }

private:
    std::vector<std::uint32_t> m_generations; ///< Текущее поколение каждого слота.
    std::vector<bool> m_alive;
    std::vector<std::uint32_t> m_free;
    std::size_t m_alive_count = 0;
};

} // namespace ECS

/// @brief Хеш для `std::unordered_map<ECS::Entity, …>`.
template<>
struct std::hash<ECS::Entity> {
    std::size_t operator()(ECS::Entity e) const noexcept {
        return std::hash<std::uint64_t>{}((std::uint64_t{e.generation} << 32) | e.index);
    }
};
