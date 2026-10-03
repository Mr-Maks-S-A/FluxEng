#pragma once
/**
 * @file Layout.hpp
 * @brief Автораскладка графа: управление слева направо, значения — слева от потребителей.
 */

#include <RuneEditor/Geometry.hpp>

namespace RuneEditor {

struct LayoutOptions {
    float column = 110.0f; ///< Расстояние между колонками.
    float row = 90.0f;     ///< Расстояние между строками.
    Vec2 origin{0.0f, 0.0f};
};

/// @brief Расставляет узлы: операторы — колонками вдоль цепочки `next`/`branch` (колонка = шаг от входа ×2), выражения — левее потребителей.
/// Узлы одной колонки идут столбиком по номерам. Возвращает число сдвинутых узлов. Смысл графа не меняется.
int auto_layout(Graph& graph, const LayoutOptions& options = {});

} // namespace RuneEditor
