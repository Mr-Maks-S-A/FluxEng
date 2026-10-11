#include <InputSystem/InputState.hpp>

#include <type_traits>

namespace InputSystem {

void InputState::begin_frame() noexcept {
    m_keys_pressed.reset();
    m_keys_released.reset();
    m_buttons_pressed.reset();
    m_buttons_released.reset();
    m_scroll = {};
    m_cursor_previous = m_cursor;
    m_text_size = 0;
    for (Pad& pad : m_pads) {
        pad.pressed.reset();
        pad.released.reset();
        pad.axes_previous = pad.axes;
    }
}

void InputState::apply_key(const KeyInput& e) noexcept {
    m_locks = e.mods & (Modifiers::CapsLock | Modifiers::NumLock);
    const std::size_t i = index_of(e.key);
    if (i >= kKeyCount || e.key == Key::Unknown) return;
    if (e.transition == Transition::Press) {
        m_keys_down.set(i);
        m_keys_pressed.set(i);
    } else if (e.transition == Transition::Release) {
        m_keys_down.reset(i);
        m_keys_released.set(i);
    } // Repeat: состояние не меняется
}

void InputState::apply_button(const MouseButtonInput& e) noexcept {
    const auto i = static_cast<std::size_t>(e.button);
    if (i >= kMouseButtonCount) return;
    if (e.transition == Transition::Press) {
        m_buttons_down.set(i);
        m_buttons_pressed.set(i);
    } else if (e.transition == Transition::Release) {
        m_buttons_down.reset(i);
        m_buttons_released.set(i);
    }
}

void InputState::apply_pad_button(const GamepadButtonInput& e) noexcept {
    const auto i = static_cast<std::size_t>(e.button);
    if (e.pad >= kMaxGamepads || i >= kGamepadButtonCount) return;
    Pad& pad = m_pads[e.pad];
    if (e.transition == Transition::Press) {
        pad.down.set(i);
        pad.pressed.set(i);
    } else if (e.transition == Transition::Release) {
        pad.down.reset(i);
        pad.released.set(i);
    }
}

void InputState::apply(const InputEvent& event) noexcept {
    std::visit(
        [this](const auto& e) noexcept {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, KeyInput>) {
                apply_key(e);
            } else if constexpr (std::is_same_v<T, MouseButtonInput>) {
                apply_button(e);
            } else if constexpr (std::is_same_v<T, CursorInput>) {
                m_cursor = {e.x, e.y};
            } else if constexpr (std::is_same_v<T, ScrollInput>) {
                m_scroll.x += e.dx;
                m_scroll.y += e.dy;
            } else if constexpr (std::is_same_v<T, CharInput>) {
                if (m_text_size < text_capacity) m_text[m_text_size++] = e.codepoint;
            } else if constexpr (std::is_same_v<T, FocusInput>) {
                m_focused = e.focused;
                if (!e.focused) release_all();
            } else if constexpr (std::is_same_v<T, GamepadConnectionInput>) {
                if (e.pad >= kMaxGamepads) return;
                Pad& pad = m_pads[e.pad];
                if (!e.connected) { // отключился: всё отпущено, оси в ноль
                    pad.released |= pad.down;
                    pad.down.reset();
                    pad.axes.fill(0.0f);
                }
                pad.connected = e.connected;
            } else if constexpr (std::is_same_v<T, GamepadButtonInput>) {
                apply_pad_button(e);
            } else if constexpr (std::is_same_v<T, GamepadAxisInput>) {
                const auto a = static_cast<std::size_t>(e.axis);
                if (e.pad < kMaxGamepads && a < kGamepadAxisCount) m_pads[e.pad].axes[a] = e.value;
            }
        },
        event);
}

void InputState::release_all() noexcept {
    m_keys_released |= m_keys_down;
    m_buttons_released |= m_buttons_down;
    m_keys_down.reset();
    m_buttons_down.reset();
    for (Pad& pad : m_pads) {
        pad.released |= pad.down;
        pad.down.reset();
    }
}

Modifiers InputState::modifiers() const noexcept {
    Modifiers mods = m_locks;
    if (down(Key::LeftShift) || down(Key::RightShift)) mods |= Modifiers::Shift;
    if (down(Key::LeftCtrl) || down(Key::RightCtrl)) mods |= Modifiers::Control;
    if (down(Key::LeftAlt) || down(Key::RightAlt)) mods |= Modifiers::Alt;
    if (down(Key::LeftSuper) || down(Key::RightSuper)) mods |= Modifiers::Super;
    return mods;
}

} // namespace InputSystem
