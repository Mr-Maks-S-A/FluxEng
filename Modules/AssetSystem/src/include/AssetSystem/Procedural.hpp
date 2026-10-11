#pragma once
/**
 * @file Procedural.hpp
 * @brief Ассеты, сгенерированные кодом: glTF/GLB-куб и BMP. Для тестов, примеров и демо, где нет файлов на диске.
 *
 * Куб 1×1×1 с центром в нуле: 24 вершины (по 4 на грань, плоские нормали), 36 индексов uint16, UV 0…1 на каждой грани.
 * Буфер можно встроить в JSON (`data:` URI) или отдать отдельным файлом `cube.bin`; из JSON + буфера собирается GLB.
 */

#include <AssetSystem/Base64.hpp>
#include <AssetSystem/Vfs.hpp>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace AssetSystem::procedural {

/// @brief Параметры куба.
struct GltfBoxOptions {
    bool embedded = true;          ///< true — буфер в `data:` URI; false — внешний файл `cube.bin`.
    bool with_normals = true;
    bool with_texture = true;
    int primitive_mode = 4;        ///< 4 = TRIANGLES, 5 = TRIANGLE_STRIP (для проверки отказа).
    float translation[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
    std::uint16_t bad_index = 0;   ///< ≠0 — подменить последний индекс (проверка защиты от индекса вне диапазона).
    std::string texture_uri = "textures/albedo.png";
    std::string buffer_uri = "cube.bin"; ///< Имя внешнего буфера (при embedded = false).
};

struct GltfBox {
    std::string json;
    Bytes bin; ///< Содержимое `cube.bin` (для embedded — то же, что внутри JSON).
};

/// Куб 1×1×1 с центром в нуле: 24 вершины (по 4 на грань), 36 индексов uint16.
inline GltfBox make_gltf_box(const GltfBoxOptions& o = {}) {
    struct V { float p[3], n[3], uv[2]; };
    std::vector<V> verts;
    std::vector<std::uint16_t> indices;
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int face = 0; face < 6; ++face) {
        const int a = face / 2;
        const float sign = (face % 2 == 0) ? 1.0f : -1.0f;
        const int u = (a + 1) % 3, v = (a + 2) % 3;
        const auto base = static_cast<std::uint16_t>(verts.size());
        for (int c = 0; c < 4; ++c) {
            const float su = (c == 1 || c == 2) ? 0.5f : -0.5f, sv = (c >= 2) ? 0.5f : -0.5f;
            V vert{};
            for (int k = 0; k < 3; ++k) {
                vert.p[k] = axes[a][k] * 0.5f * sign + axes[u][k] * su + axes[v][k] * sv;
                vert.n[k] = axes[a][k] * sign;
            }
            vert.uv[0] = su + 0.5f;
            vert.uv[1] = sv + 0.5f;
            verts.push_back(vert);
        }
        // Обход против часовой стрелки, если смотреть снаружи: (u,v,a) — правая тройка при sign>0 для чётных сдвигов.
        if (sign > 0) {
            for (const int i : {0, 1, 2, 0, 2, 3}) indices.push_back(static_cast<std::uint16_t>(base + i));
        } else {
            for (const int i : {0, 2, 1, 0, 3, 2}) indices.push_back(static_cast<std::uint16_t>(base + i));
        }
    }
    if (o.bad_index != 0) indices.back() = o.bad_index;

    Bytes bin;
    const auto append = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const std::byte*>(data);
        bin.insert(bin.end(), p, p + size);
    };
    for (const V& v : verts) append(v.p, 12);
    const std::size_t normals_at = bin.size();
    for (const V& v : verts) append(v.n, 12);
    const std::size_t uv_at = bin.size();
    for (const V& v : verts) append(v.uv, 8);
    const std::size_t index_at = bin.size();
    append(indices.data(), indices.size() * 2);

    std::string buffer_uri = o.embedded ? "data:application/octet-stream;base64," + AssetSystem::base64_encode(bin) : o.buffer_uri;
    std::string attributes = R"("POSITION":0)";
    if (o.with_normals) attributes += R"(,"NORMAL":1)";
    attributes += R"(,"TEXCOORD_0":2)";
    std::string material = o.with_texture
        ? std::format(R"("materials":[{{"pbrMetallicRoughness":{{"baseColorFactor":[1,0.5,0.25,1],"baseColorTexture":{{"index":0}}}}}}],"textures":[{{"source":0}}],"images":[{{"uri":"{}"}}],)", o.texture_uri)
        : R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0.25,1]}}],)";

    GltfBox g;
    g.bin = bin;
    g.json = std::format(
        R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],)"
        R"("nodes":[{{"mesh":0,"translation":[{},{},{}],"scale":[{},{},{}]}}],)"
        R"("meshes":[{{"primitives":[{{"attributes":{{{}}},"indices":3,"material":0,"mode":{}}}]}}],)"
        R"({})"
        R"("buffers":[{{"uri":"{}","byteLength":{}}}],)"
        R"("bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":{}}},{{"buffer":0,"byteOffset":{},"byteLength":{}}},)"
        R"({{"buffer":0,"byteOffset":{},"byteLength":{}}},{{"buffer":0,"byteOffset":{},"byteLength":{}}}],)"
        R"("accessors":[{{"bufferView":0,"componentType":5126,"count":24,"type":"VEC3","min":[-0.5,-0.5,-0.5],"max":[0.5,0.5,0.5]}},)"
        R"({{"bufferView":1,"componentType":5126,"count":24,"type":"VEC3"}},)"
        R"({{"bufferView":2,"componentType":5126,"count":24,"type":"VEC2"}},)"
        R"({{"bufferView":3,"componentType":5123,"count":36,"type":"SCALAR"}}]}})",
        o.translation[0], o.translation[1], o.translation[2], o.scale[0], o.scale[1], o.scale[2], attributes, o.primitive_mode, material,
        buffer_uri, bin.size(), normals_at, normals_at, uv_at - normals_at, uv_at, index_at - uv_at, index_at, bin.size() - index_at);
    return g;
}

/// Контейнер GLB: JSON-чанк + BIN-чанк (буфер 0 без uri).
inline Bytes make_glb(std::string json, const Bytes& bin) {
    // В GLB буфер 0 лежит в BIN-чанке и не имеет uri: убираем "uri":"…", из первого буфера.
    const std::string key = R"("buffers":[{"uri":")";
    if (const auto at = json.find(key); at != std::string::npos) {
        const auto value_end = json.find("\",", at + key.size());
        if (value_end != std::string::npos) json.erase(at + std::string(R"("buffers":[{)").size(), value_end + 2 - (at + std::string(R"("buffers":[{)").size()));
    }
    while (json.size() % 4 != 0) json += ' ';
    Bytes padded = bin;
    while (padded.size() % 4 != 0) padded.push_back(std::byte{0});
    Bytes out;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    };
    u32(0x46546C67); // "glTF"
    u32(2);
    u32(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + padded.size()));
    u32(static_cast<std::uint32_t>(json.size()));
    u32(0x4E4F534A); // "JSON"
    for (const char c : json) out.push_back(static_cast<std::byte>(c));
    u32(static_cast<std::uint32_t>(padded.size()));
    u32(0x004E4942); // "BIN\0"
    out.insert(out.end(), padded.begin(), padded.end());
    return out;
}

/// @brief Несжатый 32-битный BMP (читается stb_image): градиент по x и y, чтобы по цвету было видно ориентацию.
inline Bytes make_bmp(int width, int height) {
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(width * height * 4);
    Bytes out;
    const auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF)); };
    const auto u16 = [&](std::uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF)); };
    out.push_back(std::byte{'B'});
    out.push_back(std::byte{'M'});
    u32(54 + pixel_bytes); u32(0); u32(54);
    u32(40); u32(static_cast<std::uint32_t>(width)); u32(static_cast<std::uint32_t>(-height)); // отрицательная высота — строки сверху вниз
    u16(1); u16(32); u32(0); u32(pixel_bytes); u32(2835); u32(2835); u32(0); u32(0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) { // BGRA
            out.push_back(static_cast<std::byte>(200));
            out.push_back(static_cast<std::byte>((y * 255) / (height > 1 ? height - 1 : 1)));
            out.push_back(static_cast<std::byte>((x * 255) / (width > 1 ? width - 1 : 1)));
            out.push_back(static_cast<std::byte>(255));
        }
    }
    return out;
}

} // namespace AssetSystem::procedural
