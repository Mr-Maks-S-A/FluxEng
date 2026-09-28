#pragma once
/**
 * @file Error.hpp
 * @brief Политика ошибок модуля.
 *
 * - Ошибки **данных** (файл не найден, битое изображение, шейдер не компилируется)
 *   ожидаемы в рантайме и возвращаются как `std::expected<T, std::string>`.
 * - Ошибки **программиста** (пустой клип анимации, чужой дескриптор, нет GL-контекста)
 *   бросают RendererError.
 */

#include <stdexcept>

namespace RendererSystem {

/// @brief Ошибка использования API рендера.
class RendererError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace RendererSystem
