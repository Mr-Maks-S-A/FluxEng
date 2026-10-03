#pragma once
/**
 * @file View.hpp
 * @brief Отрисовка редактора графа рун на `Renderer2D`: круговые глифы, рёбра-кривые, порты, подсветка диагностики и исполнения.
 *
 * Только рисование: вся логика (что где лежит, что под курсором, какие проблемы) — в `Editor`, `Controller`, `Analysis`
 * и `Geometry`. Цель `engine::RuneEditorView` добавляет зависимость от `RendererSystem`; остальной модуль её не требует.
 *
 * Глиф — круг с кольцом: цвет по роду руны (данные, арифметика, контекст, чувство, управление, эффект), внутри — имя
 * руны (и число у PUSH). Кольцо показывает состояние: красное — ошибка, жёлтое — предупреждение, белое — выбран,
 * зелёное — «светится» при исполнении (`Marks::activity`, затухает со временем). Рёбра данных — голубые, управления — золотые.
 */

#include <RuneEditor/Analysis.hpp>
#include <RuneEditor/Controller.hpp>

#include <RendererSystem/RendererSystem.hpp>

#include <map>
#include <string>

namespace RuneEditor {

/// @brief Род руны для раскраски.
enum class Family : std::uint8_t { Data, Arithmetic, Context, Sense, Control, Effect };
[[nodiscard]] Family family_of(Runes::Rune rune) noexcept;
[[nodiscard]] RendererSystem::Color family_color(Family family) noexcept;
/// @brief Подпись глифа: имя руны; у PUSH — ещё и число (Q16.16 → «2.5»).
[[nodiscard]] std::string glyph_label(const GraphNode& node);

/// @brief Что дополнительно подсветить поверх графа.
struct Marks {
    const std::map<NodeId, float>* activity = nullptr; ///< Свечение исполнения 0…1 по узлам (из трассы заклинания).
    const Analysis* analysis = nullptr;                ///< Проблемы по узлам и цена.
    bool locked = false;                               ///< Редактор только для чтения (запись/повтор): правок нет, рисуется замок.
};

class View {
public:
    explicit View(RendererSystem::Renderer2D& renderer);

    /// @param area Область окна под редактор (экранные пиксели); `Controller` работает в координатах относительно её угла.
    /// @param layer Базовый слой спрайтов (редактор лежит поверх слоёв ниже).
    void draw(RendererSystem::Renderer2D& r, RendererSystem::FontHandle font, const Editor& editor, const Controller& controller, const Marks& marks,
              const RendererSystem::Rect& area, std::int32_t layer = 20) const;

private:
    RendererSystem::TextureHandle m_disc, m_ring;
};

} // namespace RuneEditor
