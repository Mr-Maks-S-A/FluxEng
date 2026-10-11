#pragma once
/**
 * @file AssetSystem.hpp
 * @brief Общий заголовок AssetSystem: `#include <AssetSystem/AssetSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | AssetSystem::AssetManager, AssetSystem::Handle — кеш, фоновая загрузка, обратные вызовы | AssetManager.hpp |
 * | AssetSystem::VirtualFileSystem, MemorySource, DirectorySource, PackSource, PackWriter | Vfs.hpp |
 * | AssetSystem::decode_image, parse_gltf, parse_mesh_binary, serialize_mesh | Loaders.hpp |
 * | AssetSystem::cook_directory — исходники → пак | Cook.hpp |
 * | AssetSystem::ImageAsset, MeshAsset, TextAsset, BlobAsset | Types.hpp |
 * | AssetSystem::AssetId, normalize_path | AssetId.hpp |
 * | AssetSystem::AssetError, Result, AssetLimits | Error.hpp |
 * | AssetSystem::procedural::make_gltf_box, make_glb, make_bmp — ассеты из кода (тесты, примеры, демо) | Procedural.hpp |
 */

#include <AssetSystem/AssetId.hpp>
#include <AssetSystem/AssetManager.hpp>
#include <AssetSystem/Base64.hpp>
#include <AssetSystem/Cook.hpp>
#include <AssetSystem/Crc32.hpp>
#include <AssetSystem/Error.hpp>
#include <AssetSystem/Loaders.hpp>
#include <AssetSystem/Procedural.hpp>
#include <AssetSystem/Types.hpp>
#include <AssetSystem/Vfs.hpp>
