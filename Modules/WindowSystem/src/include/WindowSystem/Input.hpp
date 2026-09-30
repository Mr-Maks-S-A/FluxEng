#pragma once
/**
 * @file Input.hpp
 * @brief Состояние ввода за кадр: клавиши, кнопки мыши, курсор, колесо, текст.
 *
 * InputState не зависит от GLFW и окна: окно только передаёт в него события
 * (on_key, on_mouse_button, …) и вызывает begin_frame() перед опросом событий.
 * Поэтому ввод можно тестировать и записывать/воспроизводить без окна.
 *
 * Коды клавиш и кнопок — те же, что у GLFW (`GLFW_KEY_*`, `GLFW_MOUSE_BUTTON_*`),
 * действия — `action_press` / `action_release` / `action_repeat` (совпадают с GLFW).
 *
 * | Вопрос | Метод | Когда true |
 * |---|---|---|
 * | зажата ли? | down() | с нажатия до отпускания |
 * | нажали в этом кадре? | pressed() | один кадр; ловит и нажатие+отпускание внутри кадра |
 * | отпустили в этом кадре? | released() | один кадр |
 *
 * **ZII.** `InputState{}` — «ничего не нажато, курсор в (0, 0), колесо не крутили».
 */

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>

namespace WindowSystem {

/// @brief Действие GLFW: клавиша отпущена.
inline constexpr int action_release = 0;
/// @brief Действие GLFW: клавиша нажата.
inline constexpr int action_press = 1;
/// @brief Действие GLFW: автоповтор зажатой клавиши.
inline constexpr int action_repeat = 2;

/// @brief Точка или смещение в пикселях окна.
struct Vec2d {
    double x = 0.0; ///< По горизонтали, вправо.
    double y = 0.0; ///< По вертикали, вниз.
};

/// @brief Состояние клавиатуры и мыши за текущий кадр.
class InputState {
public:
    /// @brief Число поддерживаемых кодов клавиш (GLFW_KEY_LAST = 348).
    static constexpr std::size_t key_count = 512;
    /// @brief Число кнопок мыши (GLFW_MOUSE_BUTTON_LAST = 7).
    static constexpr std::size_t button_count = 8;
    /// @brief Сколько символов текста хранится за кадр.
    static constexpr std::size_t text_capacity = 32;

    /// @brief Начало кадра: сбрасывает «нажато/отпущено в этом кадре», колесо, текст, смещение курсора.
    void begin_frame() noexcept {
        m_keys_pressed.reset();
        m_keys_released.reset();
        m_buttons_pressed.reset();
        m_buttons_released.reset();
        m_scroll = {};
        m_cursor_previous = m_cursor;
        m_text_size = 0;
    }

    // ------------------------------------------------------------------ события (их вызывает окно)

    /// @brief Событие клавиатуры. Неизвестные коды (GLFW_KEY_UNKNOWN = -1) игнорируются.
    void on_key(int key, int action) noexcept {
        if (key < 0 || static_cast<std::size_t>(key) >= key_count) return;
        apply(m_keys_down, m_keys_pressed, m_keys_released, static_cast<std::size_t>(key), action);
    }

    /// @brief Событие кнопки мыши.
    void on_mouse_button(int button, int action) noexcept {
        if (button < 0 || static_cast<std::size_t>(button) >= button_count) return;
        apply(m_buttons_down, m_buttons_pressed, m_buttons_released, static_cast<std::size_t>(button), action);
    }

    /// @brief Курсор переместился (пиксели окна, не framebuffer'а).
    void on_cursor(double x, double y) noexcept { m_cursor = {x, y}; }

    /// @brief Колесо или тачпад; за кадр смещения суммируются.
    void on_scroll(double dx, double dy) noexcept {
        m_scroll.x += dx;
        m_scroll.y += dy;
    }

    /// @brief Введён символ (Unicode). Лишние символы сверх text_capacity за кадр отбрасываются.
    void on_char(std::uint32_t codepoint) noexcept {
        if (m_text_size < text_capacity) m_text[m_text_size++] = codepoint;
    }

    /// @brief Окно потеряло фокус: отпускаем всё, иначе клавиша «залипнет».
    void on_focus_lost() noexcept {
        m_keys_released |= m_keys_down;
        m_buttons_released |= m_buttons_down;
        m_keys_down.reset();
        m_buttons_down.reset();
    }

    // ------------------------------------------------------------------ запросы

    /// @brief Клавиша зажата.
    [[nodiscard]] bool down(int key) const noexcept { return test(m_keys_down, key, key_count); }
    /// @brief Клавишу нажали в этом кадре.
    [[nodiscard]] bool pressed(int key) const noexcept { return test(m_keys_pressed, key, key_count); }
    /// @brief Клавишу отпустили в этом кадре.
    [[nodiscard]] bool released(int key) const noexcept { return test(m_keys_released, key, key_count); }

    /// @brief Кнопка мыши зажата.
    [[nodiscard]] bool mouse_down(int button) const noexcept { return test(m_buttons_down, button, button_count); }
    /// @brief Кнопку мыши нажали в этом кадре.
    [[nodiscard]] bool mouse_pressed(int button) const noexcept {
        return test(m_buttons_pressed, button, button_count);
    }
    /// @brief Кнопку мыши отпустили в этом кадре.
    [[nodiscard]] bool mouse_released(int button) const noexcept {
        return test(m_buttons_released, button, button_count);
    }

    /// @brief Курсор, пиксели окна (левый верхний угол — (0, 0)).
    [[nodiscard]] Vec2d cursor() const noexcept { return m_cursor; }
    /// @brief Смещение курсора с начала кадра.
    [[nodiscard]] Vec2d cursor_delta() const noexcept {
        return {m_cursor.x - m_cursor_previous.x, m_cursor.y - m_cursor_previous.y};
    }
    /// @brief Прокрутка за кадр (y > 0 — от себя).
    [[nodiscard]] Vec2d scroll() const noexcept { return m_scroll; }
    /// @brief Символы, введённые за кадр.
    [[nodiscard]] std::span<const std::uint32_t> text() const noexcept { return {m_text, m_text_size}; }

private:
    template<std::size_t N>
    static void apply(std::bitset<N>& down, std::bitset<N>& pressed, std::bitset<N>& released, std::size_t i,
                      int action) noexcept {
        if (action == action_press) {
            down.set(i);
            pressed.set(i);
        } else if (action == action_release) {
            down.reset(i);
            released.set(i);
        } // action_repeat: состояние не меняется
    }

    template<std::size_t N>
    [[nodiscard]] static bool test(const std::bitset<N>& bits, int i, std::size_t count) noexcept {
        return i >= 0 && static_cast<std::size_t>(i) < count && bits.test(static_cast<std::size_t>(i));
    }

    std::bitset<key_count> m_keys_down;
    std::bitset<key_count> m_keys_pressed;
    std::bitset<key_count> m_keys_released;
    std::bitset<button_count> m_buttons_down;
    std::bitset<button_count> m_buttons_pressed;
    std::bitset<button_count> m_buttons_released;
    Vec2d m_cursor;
    Vec2d m_cursor_previous;
    Vec2d m_scroll;
    std::uint32_t m_text[text_capacity] = {};
    std::size_t m_text_size = 0;
};

} // namespace WindowSystem
