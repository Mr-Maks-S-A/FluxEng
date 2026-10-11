#pragma once
/**
 * @file InputLog.hpp
 * @brief Запись ввода по кадрам и её воспроизведение: реплеи, автотесты игровой логики, воспроизведение багов.
 *
 * @code
 * InputLog log;
 * window.events().input.subscribe([&](const InputEvent& e) { log.record(frame, e); });   // запись
 * auto bytes = log.to_bytes();                                                           // в файл
 *
 * auto replay = InputLog::from_bytes(bytes).value();                                     // воспроизведение
 * for (const LoggedEvent& e : replay.events_of_frame(frame)) state.apply(e.event);
 * @endcode
 *
 * Формат двоичный, little-endian, с версией и CRC-32: числа кодов стабильны (см. Keys.hpp), поэтому запись, сделанная
 * сегодня, воспроизводится после обновления движка и на другой платформе. Разбор не доверяет данным: усечённая или
 * испорченная запись — ошибка, а не падение.
 */

#include <InputSystem/Events.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace InputSystem {

struct LoggedEvent {
    std::uint32_t frame = 0;
    InputEvent event;
    friend bool operator==(const LoggedEvent&, const LoggedEvent&) = default;
};

class InputLog {
public:
    /// @brief Добавить событие кадра `frame`. Кадры не убывают.
    /// @return false — кадр раньше последнего записанного (событие не добавлено).
    bool record(std::uint32_t frame, const InputEvent& event);

    [[nodiscard]] std::span<const LoggedEvent> events() const noexcept { return m_events; }
    /// @brief События одного кадра по порядку записи (бинарный поиск).
    [[nodiscard]] std::span<const LoggedEvent> events_of_frame(std::uint32_t frame) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_events.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_events.empty(); }
    /// @brief Кадр последнего события (0 для пустой записи).
    [[nodiscard]] std::uint32_t last_frame() const noexcept { return m_events.empty() ? 0 : m_events.back().frame; }

    [[nodiscard]] std::vector<std::byte> to_bytes() const;
    [[nodiscard]] static std::expected<InputLog, std::string> from_bytes(std::span<const std::byte> data);

    friend bool operator==(const InputLog&, const InputLog&) = default;

private:
    std::vector<LoggedEvent> m_events;
};

} // namespace InputSystem
