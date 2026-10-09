#pragma once
/**
 * @file Error.hpp
 * @brief Ошибки загрузки ассетов: код + сообщение. Ассеты приходят извне (диск, пак, сервер), поэтому любая
 * ошибка данных — это значение `std::expected`, а не исключение и не падение.
 */

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace AssetSystem {

enum class ErrorCode : std::uint8_t {
    NotFound,    ///< Нет такого файла ни в одном источнике.
    InvalidPath, ///< Путь пуст, выходит за корень (`..`), содержит `:` или управляющие символы.
    Unsupported, ///< Нет загрузчика для типа/расширения или формат использует неподдерживаемую возможность.
    Io,          ///< Ошибка чтения с диска.
    Corrupt,     ///< Данные повреждены: обрезаны, не сходится контрольная сумма, индексы вне диапазона.
    Decode,      ///< Формат разобран, но содержимое неверно (картинка не декодируется, плохой glTF).
    TooLarge,    ///< Превышен лимит (AssetLimits): файл, картинка или сетка слишком велики.
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::NotFound: return "not found";
        case ErrorCode::InvalidPath: return "invalid path";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::Io: return "io error";
        case ErrorCode::Corrupt: return "corrupt";
        case ErrorCode::Decode: return "decode error";
        case ErrorCode::TooLarge: return "too large";
    }
    return "?";
}

struct AssetError {
    ErrorCode code = ErrorCode::NotFound;
    std::string message;

    [[nodiscard]] std::string what() const { return std::string(to_string(code)) + ": " + message; }
};

template<typename T>
using Result = std::expected<T, AssetError>;

[[nodiscard]] inline std::unexpected<AssetError> fail(ErrorCode code, std::string message) {
    return std::unexpected(AssetError{code, std::move(message)});
}

/// @brief Лимиты на недоверенные данные (скины, модели и пакеты от сервера или модов).
struct AssetLimits {
    std::size_t max_file_bytes = std::size_t{256} << 20; ///< Файл целиком.
    std::uint64_t max_image_pixels = std::uint64_t{64} << 20; ///< Ширина × высота (8192² = 64 Мпикс).
    int max_image_side = 16384;                          ///< Одна сторона картинки.
    std::size_t max_vertices = std::size_t{16} << 20;    ///< Вершин в сетке.
    std::size_t max_indices = std::size_t{48} << 20;     ///< Индексов в сетке.
    std::size_t max_pack_entries = std::size_t{1} << 20; ///< Файлов в паке.
};

} // namespace AssetSystem
