#pragma once
/**
 * @file FixedStep.hpp
 * @brief Фиксированный шаг симуляции: сколько тиков выполнить за кадр.
 *
 * Кадры идут с частотой монитора, тики — с фиксированной частотой × скорость игры.
 * FixedStep накапливает время кадров и отдаёт целое число тиков; остаток — это
 * доля пути до следующего тика (alpha), по ней отрисовка может интерполировать движение.
 *
 * Не зависит от окна и GPU, поэтому тестируется отдельно (tests/test_fixed_step.cpp).
 */

namespace Core {

class FixedStep {
public:
    double ticks_per_second = 30.0; ///< Частота симуляции при скорости x1.
    int max_ticks_per_frame = 16;   ///< Защита от «спирали смерти» на медленном кадре.
    int speed = 1;                  ///< Множитель скорости (x1…x8).
    bool paused = false;            ///< Пауза: тики не идут, кадры идут.
    bool lockstep = false;          ///< Ровно один тик на кадр без учёта времени (детерминированные прогоны).

    /// @brief Длительность тика в игровых секундах (не зависит от скорости).
    [[nodiscard]] double tick_seconds() const noexcept { return 1.0 / ticks_per_second; }

    /**
     * @brief Учитывает время кадра и возвращает, сколько тиков выполнить.
     * @param frame_seconds Длительность кадра в реальных секундах.
     */
    [[nodiscard]] int advance(double frame_seconds) noexcept {
        if (paused) {
            return 0;
        }
        if (lockstep) {
            return 1;
        }
        m_accumulator += frame_seconds * speed;
        const double dt = tick_seconds();
        int steps = 0;
        while (m_accumulator >= dt && steps < max_ticks_per_frame) {
            m_accumulator -= dt;
            ++steps;
        }
        if (steps == max_ticks_per_frame) {
            m_accumulator = 0.0; // не догоняем бесконечно: лучше замедлиться, чем зависнуть
        }
        return steps;
    }

    /// @brief Доля пути от последнего тика к следующему, [0, 1). Для интерполяции отрисовки.
    [[nodiscard]] float alpha() const noexcept {
        if (lockstep) {
            return 0.0f;
        }
        return static_cast<float>(m_accumulator / tick_seconds());
    }

private:
    double m_accumulator = 0.0;
};

} // namespace Core
