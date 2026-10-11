#pragma once
/**
 * @file Loaders.hpp
 * @brief Разбор форматов в данные ассетов. Чистые функции: байты на входе, Result на выходе, без глобального состояния —
 * их можно звать из любых потоков.
 *
 * | Формат | Функция | Библиотека |
 * |---|---|---|
 * | PNG, JPG, BMP, TGA, GIF… | decode_image | stb_image |
 * | glTF 2.0 (`.gltf` + `.bin`, `.glb`) | parse_gltf | cgltf |
 * | `.fmesh` — собственный бинарный формат сетки (то, что делает cook) | parse_mesh_binary / serialize_mesh | — |
 *
 * Каждый разбор проверяет лимиты ДО выделения памяти и не доверяет данным: обрезанный, битый или враждебный файл —
 * это Result с ошибкой, а не падение.
 */

#include <AssetSystem/Error.hpp>
#include <AssetSystem/Types.hpp>
#include <AssetSystem/Vfs.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace AssetSystem {

/// @brief Декодирует картинку любого формата stb_image в RGBA8.
[[nodiscard]] Result<ImageAsset> decode_image(std::span<const std::byte> encoded, const AssetLimits& limits = {});

/// @brief Разбирает glTF/GLB в одну сетку: все узлы сцены склеены, преобразования применены, нормали достроены.
/// @param path Путь файла в VFS: от него считаются внешние `.bin`/текстуры.
/// @param vfs  Откуда читать внешние буферы; nullptr — только встроенные (`.glb`, `data:` URI).
/// Поддержаны треугольные примитивы; полосы и веера (strips/fans), точки и линии — Unsupported.
[[nodiscard]] Result<MeshAsset> parse_gltf(std::span<const std::byte> data, std::string_view path,
                                           const VirtualFileSystem* vfs, const AssetLimits& limits = {});

/// @brief Собственный бинарный формат сетки `.fmesh` (little-endian, с CRC-32 в конце).
[[nodiscard]] Bytes serialize_mesh(const MeshAsset& mesh);
[[nodiscard]] Result<MeshAsset> parse_mesh_binary(std::span<const std::byte> data, const AssetLimits& limits = {});

} // namespace AssetSystem
