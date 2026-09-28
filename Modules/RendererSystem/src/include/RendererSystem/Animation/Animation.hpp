#pragma once
/**
 * @file Animation.hpp
 * @brief Покадровая анимация спрайтов в data-oriented стиле.
 *
 * Данные разделены на две части:
 * - **клипы** (AnimationClip) — неизменяемые описания кадров, общие для всех
 *   сущностей и хранящиеся один раз в AnimationLibrary;
 * - **состояния** (AnimationState) — маленькие POD-структуры по одной на сущность
 *   (например компонент ECS).
 *
 * Все состояния обновляются одной функцией advance_animations() в плотном цикле.
 * Так тысячи анимированных существ стоят один проход по массиву, а не тысячи
 * объектов с собственными картами анимаций.
 */

#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Handles.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace RendererSystem {

/**
 * @brief Кадр анимации: область текстуры и длительность.
 */
struct AnimationFrame {
    UvRect uv{};           ///< Область спрайт-листа.
    float duration = 0.1f; ///< Длительность кадра, секунды (> 0).

    bool operator==(const AnimationFrame&) const noexcept = default;
};

/**
 * @brief Неизменяемое описание анимации.
 */
struct AnimationClip {
    std::string name;                   ///< Уникальное имя клипа, например `"goblin.walk"`.
    std::vector<AnimationFrame> frames; ///< Кадры (не пусто).
    bool looping = true;                ///< Зацикливать или остановиться на последнем кадре.

    /// @brief Суммарная длительность, секунды.
    [[nodiscard]] float total_duration() const noexcept;
};

/**
 * @brief Сетка спрайт-листа: сколько кадров по горизонтали и вертикали.
 */
struct SpriteSheetGrid {
    int columns = 1; ///< Кадров в строке.
    int rows = 1;    ///< Строк.
};

/**
 * @brief Кадры из равномерной сетки спрайт-листа.
 *
 * Кадры нумеруются слева направо, сверху вниз (строка 0 — верх листа).
 *
 * @param grid        Размер сетки.
 * @param first       Номер первого кадра.
 * @param count       Сколько кадров взять.
 * @param duration    Длительность каждого кадра, секунды.
 * @throws RendererError Если сетка пустая или кадры выходят за пределы сетки.
 */
[[nodiscard]] std::vector<AnimationFrame> make_grid_frames(SpriteSheetGrid grid, int first, int count, float duration);

/**
 * @brief Хранилище клипов. Клип добавляется один раз и дальше адресуется ClipId.
 */
class AnimationLibrary {
public:
    /**
     * @brief Добавляет клип.
     * @throws RendererError Имя пустое или занято, нет кадров, длительность кадра <= 0.
     */
    ClipId add(AnimationClip clip);

    /// @brief Поиск клипа по имени.
    [[nodiscard]] std::optional<ClipId> find(std::string_view name) const noexcept;

    /**
     * @brief Клип по дескриптору.
     * @throws RendererError Дескриптор не из этой библиотеки.
     */
    [[nodiscard]] const AnimationClip& clip(ClipId id) const;

    /// @brief Количество клипов.
    [[nodiscard]] std::size_t size() const noexcept { return m_clips.size(); }

    /// @brief `true`, если дескриптор указывает на клип этой библиотеки.
    [[nodiscard]] bool contains(ClipId id) const noexcept { return id.valid() && id.index < m_clips.size(); }

    /// @brief Клип без проверки (для горячих циклов). @pre `contains(id)`.
    [[nodiscard]] const AnimationClip& operator[](ClipId id) const noexcept { return m_clips[id.index]; }

    /// @brief Суммарная длительность клипа (кэшируется при add()). @pre `contains(id)`.
    [[nodiscard]] float duration(ClipId id) const noexcept { return m_durations[id.index]; }

private:
    std::vector<AnimationClip> m_clips;
    std::vector<float> m_durations; // кэш total_duration() по индексу клипа
};

/**
 * @brief Состояние проигрывания у одной сущности (16 байт).
 */
struct AnimationState {
    ClipId clip{};              ///< Проигрываемый клип.
    std::uint32_t frame = 0;    ///< Текущий кадр.
    float time = 0.0f;          ///< Время внутри текущего кадра, секунды.
    float speed = 1.0f;         ///< Множитель скорости (0 — пауза).

    /// @brief Состояние, начинающее клип с первого кадра.
    [[nodiscard]] static AnimationState start(ClipId clip, float speed = 1.0f) noexcept {
        return AnimationState{.clip = clip, .frame = 0, .time = 0.0f, .speed = speed};
    }
};

static_assert(sizeof(AnimationState) == 16);

/**
 * @brief Продвигает все состояния на `dt` секунд.
 *
 * - зацикленный клип при большом `dt` не крутит цикл по кадрам много раз:
 *   сначала отбрасываются целые обороты;
 * - незацикленный клип останавливается на последнем кадре (см. is_finished());
 * - состояния с невалидным клипом пропускаются.
 *
 * @pre Все валидные `clip` принадлежат `library`.
 */
void advance_animations(std::span<AnimationState> states, const AnimationLibrary& library, float dt);

/// @brief UV-область текущего кадра (UvRect{} для невалидного клипа).
[[nodiscard]] UvRect current_uv(const AnimationState& state, const AnimationLibrary& library) noexcept;

/// @brief `true`, если незацикленный клип доиграл до конца.
[[nodiscard]] bool is_finished(const AnimationState& state, const AnimationLibrary& library) noexcept;

} // namespace RendererSystem
