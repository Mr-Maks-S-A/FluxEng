#pragma once
/**
 * @file AssetId.hpp
 * @brief Нормализация путей и стабильные идентификаторы ассетов.
 *
 * Путь нормализуется один раз: `\` → `/`, лишние `//` и `.` убираются, `a/../b` → `b`. Путь, который выходит
 * выше корня (`../x`), содержит `:` (диск Windows, потоки NTFS) или управляющие символы — ошибка InvalidPath.
 * Регистр **значим** (на Linux и в паках так и есть): договоритесь о нижнем регистре в именах файлов.
 *
 * AssetId — FNV-1a 64 бита от нормализованного пути. Он одинаков на всех платформах и сборках, поэтому
 * годится для паков, сети и сохранений (в отличие от `std::hash`).
 */

#include <AssetSystem/Error.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace AssetSystem {

struct AssetId {
    std::uint64_t value = 0;
    friend constexpr bool operator==(AssetId, AssetId) noexcept = default;
    friend constexpr auto operator<=>(AssetId, AssetId) noexcept = default;
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }
};

[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

/// @brief Приводит путь к виду `dir/sub/file.ext`.
[[nodiscard]] Result<std::string> normalize_path(std::string_view raw);

/// @brief Идентификатор уже нормализованного пути.
[[nodiscard]] constexpr AssetId asset_id_of_normalized(std::string_view normalized) noexcept {
    return AssetId{fnv1a64(normalized)};
}

/// @brief Нормализует путь и берёт его идентификатор.
[[nodiscard]] Result<AssetId> asset_id(std::string_view raw);

/// @brief Расширение в нижнем регистре с точкой (`.png`); пусто, если его нет.
[[nodiscard]] std::string extension_of(std::string_view normalized_path);

/// @brief Каталог пути (`a/b/c.gltf` → `a/b`); пусто для файла в корне.
[[nodiscard]] std::string_view directory_of(std::string_view normalized_path) noexcept;

} // namespace AssetSystem

template<>
struct std::hash<AssetSystem::AssetId> {
    std::size_t operator()(AssetSystem::AssetId id) const noexcept { return static_cast<std::size_t>(id.value); }
};
