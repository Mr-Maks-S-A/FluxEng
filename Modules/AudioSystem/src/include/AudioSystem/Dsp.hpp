#pragma once
/**
 * @file Dsp.hpp
 * @brief Кирпичики обработки звука: линейная рампа, фильтр низких частот, реверберация, лимитер, интерполяция.
 *
 * Всё — обычные структуры без выделений памяти во время работы (реверберация выделяет буферы один раз в конструкторе),
 * поэтому годится для аудиопотока. Движок собирает из них шины, голоса и мастер; их можно использовать и отдельно.
 */

#include <AudioSystem/Types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AudioSystem {

/// @brief Значение, плавно идущее к цели за заданное число кадров (громкость, затухание). Без щелчков при смене.
struct Ramp {
    float value = 0.0f;
    float target = 0.0f;
    float step = 0.0f; ///< изменение за кадр (всегда ≥ 0)

    static Ramp at(float v) noexcept { return {v, v, 0.0f}; }
    /// @brief Идти к `to` за `seconds`; 0 — прыжок.
    void set(float to, float seconds, std::uint32_t sample_rate) noexcept {
        target = to;
        const float frames = seconds * static_cast<float>(sample_rate);
        if (frames < 1.0f) { value = to; step = 0.0f; return; }
        step = std::abs(to - value) / frames;
    }
    /// @brief Сдвинуть на `frames` кадров; возвращает новое значение.
    float advance(std::uint32_t frames) noexcept {
        const float delta = step * static_cast<float>(frames);
        if (value < target) value = std::min(target, value + delta);
        else value = std::max(target, value - delta);
        return value;
    }
    [[nodiscard]] bool settled() const noexcept { return value == target; }
};

/// @brief Биквад-фильтр низких частот (Q = 1/√2, Butterworth) для одного канала. Частота среза — по `set_cutoff`.
class LowPass {
public:
    void set_cutoff(float hz, std::uint32_t sample_rate) noexcept;
    [[nodiscard]] float process(float x) noexcept {
        const float y = m_b0 * x + m_z1;
        m_z1 = m_b1 * x - m_a1 * y + m_z2;
        m_z2 = m_b2 * x - m_a2 * y;
        return y;
    }
    void reset() noexcept { m_z1 = m_z2 = 0.0f; }

private:
    float m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    float m_z1 = 0.0f, m_z2 = 0.0f;
};

/// @brief Реверберация Freeverb (Jezar): 8 параллельных гребёнок и 4 all-pass на канал. Настоящий стерео-вход, стерео-выход.
class Reverb {
public:
    explicit Reverb(std::uint32_t sample_rate);

    void set(const ReverbParams& params) noexcept;
    /// @brief Обработать блок: на входе сигнал отправки, на выходе — только «мокрая» часть (в указанные буферы дописывается ничего, они заменяются).
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, std::uint32_t frames) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool active() const noexcept { return m_wet > 0.0f; }

private:
    static constexpr std::size_t comb_count = 8;
    static constexpr std::size_t allpass_count = 4;

    struct Comb {
        std::vector<float> buffer;
        std::size_t index = 0;
        float store = 0.0f;
    };
    struct Allpass {
        std::vector<float> buffer;
        std::size_t index = 0;
    };
    struct Channel {
        std::array<Comb, comb_count> combs;
        std::array<Allpass, allpass_count> allpasses;
    };

    [[nodiscard]] float run(Channel& channel, float input) noexcept;

    std::array<Channel, 2> m_channels;
    float m_feedback = 0.84f;
    float m_damp = 0.2f;
    float m_wet1 = 0.0f;
    float m_wet2 = 0.0f;
    float m_wet = 0.0f;
};

/// @brief Лимитер мастера: мгновенная атака, плавное восстановление. Гарантирует |выход| ≤ `ceiling`.
class Limiter {
public:
    explicit Limiter(std::uint32_t sample_rate, float ceiling = 0.98f, float release_seconds = 0.08f) noexcept;
    /// @brief Обработать пару отсчётов на месте; возвращает true, если сработал.
    bool process(float& left, float& right) noexcept;
    [[nodiscard]] float ceiling() const noexcept { return m_ceiling; }

private:
    float m_ceiling;
    float m_release;
    float m_envelope = 0.0f;
};

/// @brief Эрмитова (4 точки) интерполяция между y1 и y2 при дробной части `x` ∈ [0, 1).
[[nodiscard]] constexpr float hermite(float x, float y0, float y1, float y2, float y3) noexcept {
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + y1;
}

/// @brief Постоянная мощность: пан −1…+1 → коэффициенты (L, R). В центре по 1/√2 (−3 дБ), сумма мощностей всегда 1.
struct PanGains {
    float left;
    float right;
};
[[nodiscard]] PanGains pan_constant_power(float pan) noexcept;

} // namespace AudioSystem
