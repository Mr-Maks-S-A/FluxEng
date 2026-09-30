#pragma once
/**
 * @file Listeners.hpp
 * @brief Список подписчиков на событие окна: несколько слушателей вместо одного `std::function`.
 *
 * @code
 * const WindowSystem::ListenerId id = window.events().key.subscribe([](int key, int action) { ... });
 * window.events().key.unsubscribe(id);
 * @endcode
 *
 * Отписаться можно и изнутри обработчика: удаление откладывается до конца рассылки.
 * **ZII.** `ListenerId{}` (0) — «нет подписки»; отписка от него ничего не делает.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace WindowSystem {

/// @brief Номер подписки; 0 — «нет подписки».
struct ListenerId {
    std::uint32_t value = 0; ///< 0 — нет подписки.
    /// @brief `true`, если подписка была выдана.
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    /// @brief Одна и та же подписка.
    friend bool operator==(ListenerId, ListenerId) noexcept = default;
};

/**
 * @brief Подписчики события с аргументами `Args...`.
 *
 * Порядок вызова — порядок подписки.
 */
template<typename... Args>
class Listeners {
public:
    /// @brief Тип обработчика.
    using Handler = std::function<void(Args...)>;

    /// @brief Подписывает обработчик. Пустой обработчик не подписывается.
    ListenerId subscribe(Handler handler) {
        if (!handler) return {};
        const ListenerId id{++m_next_id};
        m_entries.push_back({id, std::move(handler)});
        return id;
    }

    /// @brief Отписывает. Можно вызывать и изнутри обработчика.
    /// @return `true`, если такая подписка была.
    bool unsubscribe(ListenerId id) {
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->id == id && it->handler) {
                if (m_emitting > 0) {
                    it->handler = nullptr; // удалим после рассылки
                    m_dirty = true;
                } else {
                    m_entries.erase(it); // порядок подписки сохраняется
                }
                return true;
            }
        }
        return false;
    }

    /// @brief Вызывает всех подписчиков.
    void emit(Args... args) {
        ++m_emitting;
        // По индексу: обработчик может подписать нового слушателя (он получит уже следующее событие).
        const std::size_t count = m_entries.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (m_entries[i].handler) m_entries[i].handler(args...);
        }
        if (--m_emitting == 0 && m_dirty) {
            std::erase_if(m_entries, [](const Entry& e) { return !e.handler; });
            m_dirty = false;
        }
    }

    /// @brief Число подписчиков.
    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t n = 0;
        for (const Entry& e : m_entries) n += e.handler ? 1u : 0u;
        return n;
    }
    /// @brief `true`, если подписчиков нет.
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// @brief Отписывает всех.
    void clear() {
        if (m_emitting > 0) {
            for (Entry& e : m_entries) e.handler = nullptr;
            m_dirty = true;
        } else {
            m_entries.clear();
        }
    }

private:
    struct Entry {
        ListenerId id;
        Handler handler;
    };

    std::vector<Entry> m_entries;
    std::uint32_t m_next_id = 0;
    int m_emitting = 0;
    bool m_dirty = false;
};

} // namespace WindowSystem
