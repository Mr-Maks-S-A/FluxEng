#pragma once
/**
 * @file Overlay.hpp
 * @brief Помощники оверлея поверх Renderer2D: панель текста, полоса, прицел, тепловая карта среза поля.
 */

#include <RendererSystem/RendererSystem.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace WorldRender {

/// @brief Скользящее среднее для «дрожащих» величин (FPS, мс).
struct Smoothed {
    double value = 0.0;
    double weight = 0.05;
    void add(double sample) noexcept { value = value == 0.0 ? sample : value + (sample - value) * weight; }
};

namespace overlay {

using RendererSystem::Color;
using RendererSystem::Rect;

/// @brief Панель с текстом: полупрозрачный фон по размеру строк. Возвращает нижнюю границу панели.
inline float panel(RendererSystem::Renderer2D& r, RendererSystem::FontHandle font, glm::vec2 top_left, const std::vector<std::string>& lines,
                   float text_size = 18.0f, Color text = RendererSystem::Colors::white) {
    float width = 0.0f;
    for (const std::string& line : lines) width = std::max(width, r.measure_text(font, line, {.size = text_size}).x);
    const float line_height = text_size * 1.25f;
    const float height = line_height * static_cast<float>(lines.size()) + 12.0f;
    r.fill_rect({top_left, {width + 16.0f, height}}, Color{0, 0, 0, 150}, 10);
    float y = top_left.y + 6.0f;
    for (const std::string& line : lines) {
        r.draw_text(font, line, {top_left.x + 8.0f, y}, {.size = text_size, .color = text, .layer = 11});
        y += line_height;
    }
    return top_left.y + height;
}

/// @brief Горизонтальная полоса: доля 0…1.
inline void bar(RendererSystem::Renderer2D& r, const Rect& rect, float fraction, Color fill, Color back = Color{0, 0, 0, 160}) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    r.fill_rect(rect, back, 10);
    r.fill_rect({rect.position, {rect.size.x * fraction, rect.size.y}}, fill, 11);
    r.draw_rect(rect, 1.0f, Color{255, 255, 255, 120}, 12);
}

inline void crosshair(RendererSystem::Renderer2D& r, glm::vec2 viewport, Color color = RendererSystem::Colors::white) {
    const glm::vec2 c = viewport * 0.5f;
    r.fill_rect({{c.x - 10.0f, c.y - 1.0f}, {7.0f, 2.0f}}, color, 12);
    r.fill_rect({{c.x + 3.0f, c.y - 1.0f}, {7.0f, 2.0f}}, color, 12);
    r.fill_rect({{c.x - 1.0f, c.y - 10.0f}, {2.0f, 7.0f}}, color, 12);
    r.fill_rect({{c.x - 1.0f, c.y + 3.0f}, {2.0f, 7.0f}}, color, 12);
}

/// @brief Тепловая карта `columns × rows`: `value(x, y)` ∈ [0, 1] → от тёмно-синего к жёлтому.
inline void heatmap(RendererSystem::Renderer2D& r, const Rect& rect, int columns, int rows, const std::function<float(int, int)>& value) {
    const float w = rect.size.x / static_cast<float>(columns), h = rect.size.y / static_cast<float>(rows);
    r.fill_rect({rect.position - 3.0f, rect.size + 6.0f}, Color{0, 0, 0, 170}, 10);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const float t = std::clamp(value(x, y), 0.0f, 1.0f);
            const Color c{static_cast<std::uint8_t>(30 + 225 * t * t), static_cast<std::uint8_t>(20 + 200 * t), static_cast<std::uint8_t>(110 + 60 * (1.0f - t)), 255};
            r.fill_rect({{rect.position.x + static_cast<float>(x) * w, rect.position.y + static_cast<float>(y) * h}, {w + 0.5f, h + 0.5f}}, c, 11);
        }
    }
}

} // namespace overlay
} // namespace WorldRender
