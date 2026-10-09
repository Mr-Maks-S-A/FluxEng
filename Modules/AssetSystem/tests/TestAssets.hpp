#pragma once
/**
 * @file TestAssets.hpp
 * @brief Генераторы тестовых ассетов: PNG, glTF/GLB-куб (встроенный буфер или внешний .bin).
 */

#include <AssetSystem/AssetSystem.hpp>
#include <AssetSystem/Procedural.hpp>

#include <stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace testassets {

using AssetSystem::Bytes;

inline Bytes bytes_of(std::string_view text) {
    Bytes out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

/// PNG w×h: пиксель (x,y) = (x*17, y*29, 200, 255) — по цвету видно, что строки и столбцы не перепутаны.
inline Bytes make_png(int w, int h) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w * h * 4));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = &rgba[static_cast<std::size_t>((y * w + x) * 4)];
            p[0] = static_cast<std::uint8_t>(x * 17);
            p[1] = static_cast<std::uint8_t>(y * 29);
            p[2] = 200;
            p[3] = 255;
        }
    }
    Bytes out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* bytes = static_cast<Bytes*>(ctx);
            const auto* p = static_cast<const std::byte*>(data);
            bytes->insert(bytes->end(), p, p + size);
        },
        &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

using GltfOptions = AssetSystem::procedural::GltfBoxOptions;
using Gltf = AssetSystem::procedural::GltfBox;
inline Gltf make_cube_gltf(const GltfOptions& o = {}) { return AssetSystem::procedural::make_gltf_box(o); }
inline Bytes make_glb(std::string json, const Bytes& bin) { return AssetSystem::procedural::make_glb(std::move(json), bin); }

} // namespace testassets
