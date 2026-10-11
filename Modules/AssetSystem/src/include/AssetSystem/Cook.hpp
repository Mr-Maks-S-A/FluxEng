#pragma once
/**
 * @file Cook.hpp
 * @brief Cooker: исходные ассеты → пак для рантайма.
 *
 * glTF/GLB разбираются один раз при сборке и пишутся как `.fmesh` (вершины уже в формате рендера, без JSON и base64),
 * остальные файлы копируются как есть. Файлы `.bin` — буферы glTF — в пак не попадают (их данные уже внутри `.fmesh`).
 * Результат не зависит от порядка обхода каталога: файлы сортируются по пути.
 */

#include <AssetSystem/Error.hpp>
#include <AssetSystem/Vfs.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace AssetSystem {

struct CookReport {
    std::size_t files = 0;          ///< Записей в паке.
    std::size_t meshes_cooked = 0;  ///< glTF → .fmesh.
    std::size_t bytes_in = 0;       ///< Размер исходников, прочитанных cooker'ом.
    std::size_t bytes_out = 0;      ///< Размер записей, добавленных в пак.
    std::vector<std::string> warnings;
};

/// @brief Обходит каталог `source` и добавляет в `pack` готовые ассеты.
/// Первая же ошибка разбора прерывает работу: битый исходник не должен молча выпасть из пака.
[[nodiscard]] Result<CookReport> cook_directory(const std::filesystem::path& source, PackWriter& pack,
                                                const AssetLimits& limits = {});

} // namespace AssetSystem
