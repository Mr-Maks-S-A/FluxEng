#include <AssetSystem/AssetId.hpp>
#include <AssetSystem/Loaders.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wold-style-cast"
#    pragma GCC diagnostic ignored "-Wcast-qual"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#    pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace AssetSystem {

namespace {

std::string describe(cgltf_result result) {
    switch (result) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data too short";
        case cgltf_result_unknown_format: return "unknown format";
        case cgltf_result_invalid_json: return "invalid JSON";
        case cgltf_result_invalid_gltf: return "invalid glTF";
        case cgltf_result_invalid_options: return "invalid options";
        case cgltf_result_file_not_found: return "referenced file not found";
        case cgltf_result_io_error: return "io error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "legacy glTF 1.0 is not supported";
        default: return "unknown error";
    }
}

// Внешние файлы glTF (.bin) cgltf читает через VFS, а не через fopen: пак, каталог и память работают одинаково.
cgltf_result vfs_read(const cgltf_memory_options* memory, const cgltf_file_options* options, const char* path, cgltf_size* size,
                      void** data) {
    const auto* vfs = static_cast<const VirtualFileSystem*>(options->user_data);
    if (vfs == nullptr) return cgltf_result_file_not_found;
    const auto bytes = vfs->read(path);
    if (!bytes) return cgltf_result_file_not_found;
    void* block = memory->alloc_func != nullptr ? memory->alloc_func(memory->user_data, bytes->size()) : std::malloc(bytes->size());
    if (block == nullptr && !bytes->empty()) return cgltf_result_out_of_memory;
    if (!bytes->empty()) std::memcpy(block, bytes->data(), bytes->size());
    if (size != nullptr) *size = bytes->size();
    *data = block;
    return cgltf_result_success;
}

void vfs_release(const cgltf_memory_options* memory, const cgltf_file_options*, void* data) {
    if (memory->free_func != nullptr) {
        memory->free_func(memory->user_data, data);
    } else {
        std::free(data);
    }
}

struct Data {
    cgltf_data* gltf = nullptr;
    ~Data() {
        if (gltf != nullptr) cgltf_free(gltf);
    }
};

std::uint8_t to_byte(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

struct Mat3 {
    float m[9];
};

// Нормали преобразуются присоединённой матрицей (кофакторами) верхней 3×3; знак определителя учитывает отражение.
Mat3 normal_matrix(const float* w, float& determinant) {
    // w — столбцы: w[0..2] первый столбец и т.д.
    const float a = w[0], b = w[4], c = w[8];   // строка 0: m00 m01 m02
    const float d = w[1], e = w[5], f = w[9];   // строка 1
    const float g = w[2], h = w[6], i = w[10];  // строка 2
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    Mat3 n{};
    const float s = determinant < 0.0f ? -1.0f : 1.0f;
    // Кофакторная матрица (она же det·(M⁻¹)ᵀ), строки.
    n.m[0] = s * (e * i - f * h); n.m[1] = s * (f * g - d * i); n.m[2] = s * (d * h - e * g);
    n.m[3] = s * (c * h - b * i); n.m[4] = s * (a * i - c * g); n.m[5] = s * (b * g - a * h);
    n.m[6] = s * (b * f - c * e); n.m[7] = s * (c * d - a * f); n.m[8] = s * (a * e - b * d);
    return n;
}

void smooth_normals(MeshAsset& mesh, std::size_t first_vertex, std::size_t first_index) {
    for (std::size_t v = first_vertex; v < mesh.vertices.size(); ++v) {
        std::fill(std::begin(mesh.vertices[v].normal), std::end(mesh.vertices[v].normal), 0.0f);
    }
    for (std::size_t t = first_index; t + 2 < mesh.indices.size(); t += 3) {
        MeshVertex& p0 = mesh.vertices[mesh.indices[t]];
        MeshVertex& p1 = mesh.vertices[mesh.indices[t + 1]];
        MeshVertex& p2 = mesh.vertices[mesh.indices[t + 2]];
        float e1[3], e2[3];
        for (int k = 0; k < 3; ++k) {
            e1[k] = p1.position[k] - p0.position[k];
            e2[k] = p2.position[k] - p0.position[k];
        }
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (MeshVertex* p : {&p0, &p1, &p2}) {
            for (int k = 0; k < 3; ++k) p->normal[k] += n[k]; // длина = удвоенная площадь: взвешивание по площади
        }
    }
    for (std::size_t v = first_vertex; v < mesh.vertices.size(); ++v) {
        float* n = mesh.vertices[v].normal;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-20f) {
            for (int k = 0; k < 3; ++k) n[k] /= len;
        } else {
            n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
        }
    }
}

Result<void> append_primitive(MeshAsset& mesh, const cgltf_primitive& prim, const float* world, const AssetLimits& limits) {
    if (prim.type != cgltf_primitive_type_triangles) {
        return fail(ErrorCode::Unsupported, "only triangle primitives are supported (no strips, fans, lines or points)");
    }
    const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr, *color = nullptr;
    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& attribute = prim.attributes[a];
        if (attribute.index != 0) continue;
        switch (attribute.type) {
            case cgltf_attribute_type_position: position = attribute.data; break;
            case cgltf_attribute_type_normal: normal = attribute.data; break;
            case cgltf_attribute_type_texcoord: uv = attribute.data; break;
            case cgltf_attribute_type_color: color = attribute.data; break;
            default: break;
        }
    }
    if (position == nullptr || position->type != cgltf_type_vec3) return fail(ErrorCode::Decode, "primitive has no VEC3 POSITION");
    const std::size_t count = position->count;
    if (count > limits.max_vertices || mesh.vertices.size() > limits.max_vertices - count) return fail(ErrorCode::TooLarge, "mesh has too many vertices");
    if ((normal != nullptr && (normal->type != cgltf_type_vec3 || normal->count != count)) ||
        (uv != nullptr && (uv->type != cgltf_type_vec2 || uv->count != count)) ||
        (color != nullptr && ((color->type != cgltf_type_vec3 && color->type != cgltf_type_vec4) || color->count != count))) {
        return fail(ErrorCode::Decode, "primitive attributes disagree on vertex count or type");
    }

    const std::size_t index_count = prim.indices != nullptr ? prim.indices->count : count;
    if (index_count % 3 != 0) return fail(ErrorCode::Decode, "index count is not a multiple of 3");
    if (index_count > limits.max_indices || mesh.indices.size() > limits.max_indices - index_count) return fail(ErrorCode::TooLarge, "mesh has too many indices");

    float determinant = 1.0f;
    const Mat3 nm = normal_matrix(world, determinant);
    const std::size_t first_vertex = mesh.vertices.size();
    const std::size_t first_index = mesh.indices.size();
    mesh.vertices.resize(first_vertex + count);

    for (std::size_t i = 0; i < count; ++i) {
        MeshVertex& out = mesh.vertices[first_vertex + i];
        float p[3] = {};
        if (!cgltf_accessor_read_float(position, i, p, 3)) return fail(ErrorCode::Decode, "cannot read POSITION");
        for (int r = 0; r < 3; ++r) out.position[r] = world[r] * p[0] + world[4 + r] * p[1] + world[8 + r] * p[2] + world[12 + r];
        if (normal != nullptr) {
            float n[3] = {};
            if (!cgltf_accessor_read_float(normal, i, n, 3)) return fail(ErrorCode::Decode, "cannot read NORMAL");
            float t[3];
            for (int r = 0; r < 3; ++r) t[r] = nm.m[r * 3] * n[0] + nm.m[r * 3 + 1] * n[1] + nm.m[r * 3 + 2] * n[2];
            const float len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            for (int r = 0; r < 3; ++r) out.normal[r] = len > 1e-20f ? t[r] / len : (r == 1 ? 1.0f : 0.0f);
        }
        if (uv != nullptr && !cgltf_accessor_read_float(uv, i, out.uv, 2)) return fail(ErrorCode::Decode, "cannot read TEXCOORD_0");
        if (color != nullptr) {
            float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (!cgltf_accessor_read_float(color, i, c, cgltf_num_components(color->type))) return fail(ErrorCode::Decode, "cannot read COLOR_0");
            out.rgba = static_cast<std::uint32_t>(to_byte(c[0])) | (static_cast<std::uint32_t>(to_byte(c[1])) << 8) |
                       (static_cast<std::uint32_t>(to_byte(c[2])) << 16) | (static_cast<std::uint32_t>(to_byte(c[3])) << 24);
        }
    }

    mesh.indices.resize(first_index + index_count);
    for (std::size_t i = 0; i < index_count; ++i) {
        const std::size_t index = prim.indices != nullptr ? cgltf_accessor_read_index(prim.indices, i) : i;
        if (index >= count) return fail(ErrorCode::Corrupt, "primitive index is out of range");
        mesh.indices[first_index + i] = static_cast<std::uint32_t>(first_vertex + index);
    }
    if (determinant < 0.0f) { // отражение переворачивает обход: возвращаем лицевую сторону
        for (std::size_t t = first_index; t + 2 < mesh.indices.size(); t += 3) std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
    }
    if (normal == nullptr) smooth_normals(mesh, first_vertex, first_index);
    return {};
}

void take_material(MeshAsset& mesh, const cgltf_material* material, std::string_view gltf_path, bool& taken) {
    if (taken || material == nullptr || !material->has_pbr_metallic_roughness) return;
    taken = true;
    const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
    for (int k = 0; k < 4; ++k) mesh.base_color[k] = pbr.base_color_factor[k];
    const cgltf_texture* texture = pbr.base_color_texture.texture;
    if (texture == nullptr || texture->image == nullptr || texture->image->uri == nullptr) return;
    std::string uri = texture->image->uri;
    if (uri.starts_with("data:")) return; // встроенные картинки — через .glb/bufferView: не поддержано, цвет без текстуры
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    const std::string_view dir = directory_of(gltf_path);
    const auto resolved = normalize_path(dir.empty() ? uri : std::string(dir) + "/" + uri);
    if (resolved) mesh.base_color_texture = *resolved;
}

} // namespace

Result<MeshAsset> parse_gltf(std::span<const std::byte> data, std::string_view path, const VirtualFileSystem* vfs,
                             const AssetLimits& limits) {
    if (data.empty()) return fail(ErrorCode::Decode, "empty glTF data");
    if (data.size() > limits.max_file_bytes) return fail(ErrorCode::TooLarge, "glTF file is larger than the limit");

    cgltf_options options{};
    options.file.read = &vfs_read;
    options.file.release = &vfs_release;
    options.file.user_data = const_cast<VirtualFileSystem*>(vfs); // NOLINT: cgltf хранит void*, мы читаем только const-методы

    Data parsed;
    if (const cgltf_result r = cgltf_parse(&options, data.data(), data.size(), &parsed.gltf); r != cgltf_result_success) {
        return fail(ErrorCode::Decode, "glTF parse: " + describe(r));
    }
    // Размеры буферов — до чтения: количество вершин и индексов ограничено лимитами ниже, а не данными файла.
    const std::string path_string(path);
    if (const cgltf_result r = cgltf_load_buffers(&options, parsed.gltf, path_string.c_str()); r != cgltf_result_success) {
        return fail(r == cgltf_result_file_not_found ? ErrorCode::NotFound : ErrorCode::Decode, "glTF buffers: " + describe(r));
    }
    if (const cgltf_result r = cgltf_validate(parsed.gltf); r != cgltf_result_success) {
        return fail(ErrorCode::Decode, "glTF validation: " + describe(r));
    }

    MeshAsset mesh;
    bool material_taken = false;
    bool any_node = false;
    for (cgltf_size n = 0; n < parsed.gltf->nodes_count; ++n) {
        const cgltf_node& node = parsed.gltf->nodes[n];
        if (node.mesh == nullptr) continue;
        any_node = true;
        float world[16];
        cgltf_node_transform_world(&node, world);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (auto r = append_primitive(mesh, prim, world, limits); !r) return std::unexpected(std::move(r.error()));
            take_material(mesh, prim.material, path, material_taken);
        }
    }
    if (!any_node) { // меши есть, а узлов с ними нет — берём без преобразований
        static constexpr float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (cgltf_size m = 0; m < parsed.gltf->meshes_count; ++m) {
            for (cgltf_size p = 0; p < parsed.gltf->meshes[m].primitives_count; ++p) {
                const cgltf_primitive& prim = parsed.gltf->meshes[m].primitives[p];
                if (auto r = append_primitive(mesh, prim, identity, limits); !r) return std::unexpected(std::move(r.error()));
                take_material(mesh, prim.material, path, material_taken);
            }
        }
    }
    if (mesh.indices.empty()) return fail(ErrorCode::Decode, "glTF contains no triangles");
    mesh.compute_bounds();
    return mesh;
}

} // namespace AssetSystem
