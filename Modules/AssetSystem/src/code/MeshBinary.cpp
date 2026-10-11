#include <AssetSystem/AssetId.hpp>
#include <AssetSystem/Crc32.hpp>
#include <AssetSystem/Loaders.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace AssetSystem {

static_assert(std::endian::native == std::endian::little, "AssetSystem formats are little-endian");

void MeshAsset::compute_bounds() noexcept {
    if (vertices.empty()) {
        std::fill(std::begin(bounds_min), std::end(bounds_min), 0.0f);
        std::fill(std::begin(bounds_max), std::end(bounds_max), 0.0f);
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        bounds_min[axis] = vertices[0].position[axis];
        bounds_max[axis] = vertices[0].position[axis];
    }
    for (const MeshVertex& v : vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            bounds_min[axis] = std::min(bounds_min[axis], v.position[axis]);
            bounds_max[axis] = std::max(bounds_max[axis], v.position[axis]);
        }
    }
}

namespace {

constexpr char kMagic[4] = {'F', 'X', 'M', 'S'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxTexturePath = 4096;

template<typename T>
void put(Bytes& out, const T& value) {
    const std::size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

void put_raw(Bytes& out, const void* data, std::size_t size) {
    if (size == 0) return;
    const std::size_t at = out.size();
    out.resize(at + size);
    std::memcpy(out.data() + at, data, size);
}

} // namespace

Bytes serialize_mesh(const MeshAsset& mesh) {
    Bytes out;
    out.reserve(32 + mesh.base_color_texture.size() + mesh.size_bytes());
    put_raw(out, kMagic, 4);
    put(out, kVersion);
    put(out, static_cast<std::uint32_t>(mesh.vertices.size()));
    put(out, static_cast<std::uint32_t>(mesh.indices.size()));
    put_raw(out, mesh.base_color, sizeof(mesh.base_color));
    put(out, static_cast<std::uint32_t>(mesh.base_color_texture.size()));
    put_raw(out, mesh.base_color_texture.data(), mesh.base_color_texture.size());
    put_raw(out, mesh.vertices.data(), mesh.vertices.size() * sizeof(MeshVertex));
    put_raw(out, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    put(out, crc32(out));
    return out;
}

Result<MeshAsset> parse_mesh_binary(std::span<const std::byte> data, const AssetLimits& limits) {
    constexpr std::size_t header = 4 + 4 + 4 + 4 + 16 + 4;
    if (data.size() < header + 4) return fail(ErrorCode::Corrupt, "mesh file is truncated");
    if (std::memcmp(data.data(), kMagic, 4) != 0) return fail(ErrorCode::Corrupt, "not a FXMS mesh (bad magic)");

    // Контрольная сумма — первой: всё, что дальше, читается из данных, в целостности которых мы уверены.
    std::uint32_t stored = 0;
    std::memcpy(&stored, data.data() + data.size() - 4, 4);
    if (crc32(data.first(data.size() - 4)) != stored) return fail(ErrorCode::Corrupt, "mesh checksum mismatch");

    std::size_t pos = 4;
    const auto read = [&](void* dst, std::size_t size) {
        std::memcpy(dst, data.data() + pos, size);
        pos += size;
    };
    std::uint32_t version = 0, vertex_count = 0, index_count = 0, texture_len = 0;
    MeshAsset mesh;
    read(&version, 4);
    read(&vertex_count, 4);
    read(&index_count, 4);
    read(mesh.base_color, sizeof(mesh.base_color));
    read(&texture_len, 4);
    if (version != kVersion) return fail(ErrorCode::Unsupported, "mesh version " + std::to_string(version));
    if (vertex_count > limits.max_vertices || index_count > limits.max_indices) return fail(ErrorCode::TooLarge, "mesh exceeds the limit");
    if (index_count % 3 != 0) return fail(ErrorCode::Corrupt, "index count is not a multiple of 3");
    if (texture_len > kMaxTexturePath) return fail(ErrorCode::Corrupt, "texture path is too long");

    const std::size_t body = static_cast<std::size_t>(texture_len) + static_cast<std::size_t>(vertex_count) * sizeof(MeshVertex) +
                             static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);
    if (data.size() - header - 4 != body) return fail(ErrorCode::Corrupt, "mesh size does not match its header");

    mesh.base_color_texture.assign(reinterpret_cast<const char*>(data.data() + pos), texture_len);
    pos += texture_len;
    if (!mesh.base_color_texture.empty()) {
        const auto normalized = normalize_path(mesh.base_color_texture);
        if (!normalized || *normalized != mesh.base_color_texture) return fail(ErrorCode::Corrupt, "mesh has a bad texture path");
    }
    mesh.vertices.resize(vertex_count);
    read(mesh.vertices.data(), static_cast<std::size_t>(vertex_count) * sizeof(MeshVertex));
    mesh.indices.resize(index_count);
    read(mesh.indices.data(), static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));

    for (const std::uint32_t index : mesh.indices) {
        if (index >= vertex_count) return fail(ErrorCode::Corrupt, "mesh index is out of range");
    }
    mesh.compute_bounds();
    return mesh;
}

} // namespace AssetSystem
