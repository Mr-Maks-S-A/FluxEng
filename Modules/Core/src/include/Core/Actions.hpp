#pragma once
/**
 * @file Actions.hpp
 * @brief Ввод через действия: «прыжок», «каст из окружающей маны» — вместо проверок клавиш по всему коду игры.
 *
 * Игра объявляет **действия** по смыслу и привязывает к ним клавиши и кнопки мыши. Дальше она спрашивает
 * `actions.pressed(input, jump)` и не знает, какая клавиша за этим стоит. Это даёт:
 * - **перепривязку**: игрок меняет клавиши в текстовом файле, код игры не трогается;
 * - **несколько клавиш на действие** (W и стрелка вверх), оси из двух действий (`axis`);
 * - **проверку конфликтов**: `conflicts(action)` — какие ещё действия используют ту же клавишу;
 * - единое место для подсказок в интерфейсе (`binding_name`) и для будущих геймпадов.
 *
 * @code
 * Core::ActionMap actions;
 * const auto jump  = actions.declare("jump", "Прыжок");
 * const auto fwd   = actions.declare("move_forward");
 * const auto back  = actions.declare("move_back");
 * actions.bind(jump, Core::Binding::key(GLFW_KEY_SPACE));
 * actions.bind(fwd,  Core::Binding::key(GLFW_KEY_W)).bind(fwd, Core::Binding::key(GLFW_KEY_UP));
 * actions.bind(back, Core::Binding::key(GLFW_KEY_S));
 * actions.apply("jump = SPACE, MOUSE_RIGHT\n");        // файл игрока переопределяет привязки
 *
 * // каждый кадр:
 * if (actions.pressed(window.input(), jump)) start_jump();
 * const float walk = actions.axis(window.input(), fwd, back);    // −1, 0 или +1
 * @endcode
 *
 * Работает поверх `WindowSystem::InputState` (его заполняет окно; в тестах — вручную через `on_key`).
 */

#include <WindowSystem/Input.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Core {

/// @brief Клавиша или кнопка мыши.
struct Binding {
    enum class Device : std::uint8_t { Key, Mouse };
    Device device = Device::Key;
    int code = 0; ///< Код GLFW (клавиша) или номер кнопки мыши.

    [[nodiscard]] static constexpr Binding key(int glfw_key) noexcept { return {Device::Key, glfw_key}; }
    [[nodiscard]] static constexpr Binding mouse(int button) noexcept { return {Device::Mouse, button}; }
    [[nodiscard]] friend constexpr bool operator==(Binding, Binding) noexcept = default;
};

/// @brief Имя привязки для файла настроек и подсказок: «W», «SPACE», «F5», «MOUSE_LEFT». Неизвестный код — «KEY_<код>».
[[nodiscard]] std::string binding_name(Binding binding);
/// @brief Привязка по имени (без учёта регистра); `nullopt`, если имени нет в таблице.
[[nodiscard]] std::optional<Binding> parse_binding(std::string_view name);

/// @brief Номер действия внутри `ActionMap`. Получается из `declare`; нулевой «пустой» не существует.
struct ActionId {
    std::uint16_t value = 0xFFFF;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0xFFFF; }
    [[nodiscard]] friend constexpr bool operator==(ActionId, ActionId) noexcept = default;
};

class ActionMap {
public:
    /// @brief Объявляет действие. Повторное имя возвращает прежний номер (описание дополняется, если было пустым).
    ActionId declare(std::string name, std::string description = {});
    [[nodiscard]] std::optional<ActionId> find(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_actions.size(); }
    [[nodiscard]] const std::string& name(ActionId id) const { return m_actions.at(id.value).name; }
    [[nodiscard]] const std::string& description(ActionId id) const { return m_actions.at(id.value).description; }

    /// @brief Добавляет привязку (повтор той же привязки игнорируется).
    ActionMap& bind(ActionId id, Binding binding);
    /// @brief Убирает одну привязку; `false`, если её не было.
    bool unbind(ActionId id, Binding binding);
    /// @brief Убирает все привязки действия (перед перепривязкой).
    void unbind_all(ActionId id);
    [[nodiscard]] std::span<const Binding> bindings(ActionId id) const { return m_actions.at(id.value).bindings; }
    /// @brief Другие действия, использующие любую из привязок этого.
    [[nodiscard]] std::vector<ActionId> conflicts(ActionId id) const;

    // ---- Запросы к состоянию ввода текущего кадра: достаточно сработать любой из привязок действия ----
    [[nodiscard]] bool down(const WindowSystem::InputState& input, ActionId id) const noexcept;
    [[nodiscard]] bool pressed(const WindowSystem::InputState& input, ActionId id) const noexcept;
    [[nodiscard]] bool released(const WindowSystem::InputState& input, ActionId id) const noexcept;
    /// @brief Ось из двух действий: +1, если зажато `positive`, −1, если `negative`, 0 — оба или ни одного.
    [[nodiscard]] float axis(const WindowSystem::InputState& input, ActionId positive, ActionId negative) const noexcept;

    /**
     * @brief Читает привязки из текста и заменяет ими привязки **упомянутых** действий (остальные не трогаются).
     *
     * Формат: по строке на действие, `имя = КЛАВИША, КЛАВИША`, комментарии с `#`, пустая правая часть — действие
     * без привязок. Ошибка (неизвестное действие или клавиша) с номером строки — и ничего не применяется.
     */
    [[nodiscard]] std::expected<void, std::string> apply(std::string_view text);
    /// @brief Текст в формате `apply` со всеми действиями и описаниями в комментариях — готовый файл настроек.
    [[nodiscard]] std::string serialize() const;

private:
    struct Action {
        std::string name;
        std::string description;
        std::vector<Binding> bindings;
    };
    [[nodiscard]] static bool test(const WindowSystem::InputState& input, Binding b, int kind) noexcept;
    template<typename Pred>
    [[nodiscard]] bool any(ActionId id, Pred&& pred) const noexcept;

    std::vector<Action> m_actions;
};

} // namespace Core
