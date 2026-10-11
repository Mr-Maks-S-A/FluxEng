/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки AssetSystem (google-benchmark).
 *
 * Группы:
 * 1. Мелочи: нормализация пути, AssetId, CRC-32.
 * 2. Чтение: память против пака (пак проверяет CRC каждой записи).
 * 3. Разбор: PNG, glTF с base64-буфером против собственного `.fmesh` той же сетки — зачем нужен cooker.
 * 4. Менеджер: попадание в кеш, поток асинхронных загрузок при 0…N потоках.
 *
 * Для осмысленных цифр собирайте в Release.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <AssetSystem/AssetSystem.hpp>

#include <benchmark/benchmark.h>

#include <cmath>
#include <cstring>
#include <format>
#include <string>

namespace {

namespace as = AssetSystem;
using as::Bytes;

Bytes make_png(int w, int h) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w * h * 4));
    for (int i = 0; i < w * h; ++i) {
        rgba[static_cast<std::size_t>(i * 4)] = static_cast<std::uint8_t>(i);
        rgba[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>(i >> 3);
        rgba[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>(i * 7);
        rgba[static_cast<std::size_t>(i * 4 + 3)] = 255;
    }
    Bytes out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* bytes = static_cast<Bytes*>(ctx);
        const auto* p = static_cast<const std::byte*>(data);
        bytes->insert(bytes->end(), p, p + size);
    }, &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

/// glTF сетки-сетки n×n квадратов в плоскости XZ: (n+1)² вершин, 2n² треугольников, индексы uint32.
std::string make_grid_gltf(int n) {
    const int side = n + 1;
    std::vector<float> positions, normals, uvs;
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(n), fz = static_cast<float>(z) / static_cast<float>(n);
            positions.insert(positions.end(), {fx, std::sin(fx * 6.0f) * 0.1f, fz});
            normals.insert(normals.end(), {0.0f, 1.0f, 0.0f});
            uvs.insert(uvs.end(), {fx, fz});
        }
    }
    std::vector<std::uint32_t> indices;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const auto i = static_cast<std::uint32_t>(z * side + x);
            const auto s = static_cast<std::uint32_t>(side);
            indices.insert(indices.end(), {i, i + s, i + 1, i + 1, i + s, i + s + 1});
        }
    }
    Bytes bin;
    const auto append = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const std::byte*>(data);
        bin.insert(bin.end(), p, p + size);
    };
    append(positions.data(), positions.size() * 4);
    const std::size_t normals_at = bin.size();
    append(normals.data(), normals.size() * 4);
    const std::size_t uv_at = bin.size();
    append(uvs.data(), uvs.size() * 4);
    const std::size_t index_at = bin.size();
    append(indices.data(), indices.size() * 4);
    const std::size_t count = positions.size() / 3;
    return std::format(
        R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0}}],)"
        R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2}},"indices":3}}]}}],)"
        R"("buffers":[{{"uri":"data:application/octet-stream;base64,{}","byteLength":{}}}],)"
        R"("bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":{}}},{{"buffer":0,"byteOffset":{},"byteLength":{}}},)"
        R"({{"buffer":0,"byteOffset":{},"byteLength":{}}},{{"buffer":0,"byteOffset":{},"byteLength":{}}}],)"
        R"("accessors":[{{"bufferView":0,"componentType":5126,"count":{},"type":"VEC3","min":[0,-0.1,0],"max":[1,0.1,1]}},)"
        R"({{"bufferView":1,"componentType":5126,"count":{},"type":"VEC3"}},{{"bufferView":2,"componentType":5126,"count":{},"type":"VEC2"}},)"
        R"({{"bufferView":3,"componentType":5125,"count":{},"type":"SCALAR"}}]}})",
        as::base64_encode(bin), bin.size(), normals_at, normals_at, uv_at - normals_at, uv_at, index_at - uv_at, index_at,
        bin.size() - index_at, count, count, count, indices.size());
}

Bytes bytes_of(const std::string& s) {
    Bytes b(s.size());
    std::memcpy(b.data(), s.data(), s.size());
    return b;
}

// -----------------------------------------------------------------------------
// 1. Мелочи
// -----------------------------------------------------------------------------

void BM_NormalizePath(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(as::normalize_path("./models\\\\heroes//mage/../mage/staff_01.fmesh"));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NormalizePath);

void BM_AssetId(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(as::asset_id_of_normalized("models/heroes/mage/staff_01.fmesh"));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AssetId);

void BM_Crc32(benchmark::State& state) {
    Bytes data(static_cast<std::size_t>(state.range(0)), std::byte{0x5A});
    for (auto _ : state) benchmark::DoNotOptimize(as::crc32(data));
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Crc32)->Arg(1 << 10)->Arg(1 << 20);

// -----------------------------------------------------------------------------
// 2. Чтение
// -----------------------------------------------------------------------------

void BM_Vfs_ReadMemory(benchmark::State& state) {
    auto source = std::make_unique<as::MemorySource>();
    source->add("data/blob.dat", Bytes(static_cast<std::size_t>(state.range(0)), std::byte{1}));
    as::VirtualFileSystem vfs;
    vfs.mount(std::move(source));
    for (auto _ : state) benchmark::DoNotOptimize(vfs.read("data/blob.dat"));
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Vfs_ReadMemory)->Arg(1 << 10)->Arg(1 << 20);

void BM_Vfs_ReadPack(benchmark::State& state) {
    as::PackWriter writer;
    writer.add("data/blob.dat", Bytes(static_cast<std::size_t>(state.range(0)), std::byte{1}));
    as::VirtualFileSystem vfs;
    vfs.mount(std::move(*as::PackSource::open(*writer.build())));
    for (auto _ : state) benchmark::DoNotOptimize(vfs.read("data/blob.dat"));
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Vfs_ReadPack)->Arg(1 << 10)->Arg(1 << 20);

// -----------------------------------------------------------------------------
// 3. Разбор
// -----------------------------------------------------------------------------

void BM_DecodePng(benchmark::State& state) {
    const Bytes png = make_png(static_cast<int>(state.range(0)), static_cast<int>(state.range(0)));
    for (auto _ : state) benchmark::DoNotOptimize(as::decode_image(png));
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * state.range(0) * state.range(0) * 4);
}
BENCHMARK(BM_DecodePng)->Arg(64)->Arg(256)->Arg(1024);

// Одна и та же сетка (n×n квадратов): glTF с base64 и после cook. Разница — выигрыш cooker'а при запуске игры.
void BM_ParseGltf(benchmark::State& state) {
    const Bytes json = bytes_of(make_grid_gltf(static_cast<int>(state.range(0))));
    std::size_t triangles = 0;
    for (auto _ : state) {
        auto mesh = as::parse_gltf(json, "grid.gltf", nullptr);
        triangles = mesh->triangle_count();
        benchmark::DoNotOptimize(mesh);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(triangles));
}
BENCHMARK(BM_ParseGltf)->Arg(32)->Arg(128)->Arg(256);

void BM_ParseMeshBinary(benchmark::State& state) {
    const Bytes json = bytes_of(make_grid_gltf(static_cast<int>(state.range(0))));
    const Bytes cooked = as::serialize_mesh(*as::parse_gltf(json, "grid.gltf", nullptr));
    std::size_t triangles = 0;
    for (auto _ : state) {
        auto mesh = as::parse_mesh_binary(cooked);
        triangles = mesh->triangle_count();
        benchmark::DoNotOptimize(mesh);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(triangles));
}
BENCHMARK(BM_ParseMeshBinary)->Arg(32)->Arg(128)->Arg(256);

// -----------------------------------------------------------------------------
// 4. Менеджер
// -----------------------------------------------------------------------------

void BM_Manager_CacheHit(benchmark::State& state) {
    as::VirtualFileSystem vfs;
    auto source = std::make_unique<as::MemorySource>();
    source->add("img/a.png", make_png(16, 16));
    vfs.mount(std::move(source));
    as::AssetManager assets(vfs);
    const auto keep = assets.load<as::ImageAsset>("img/a.png");
    for (auto _ : state) benchmark::DoNotOptimize(assets.load<as::ImageAsset>("img/a.png"));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Manager_CacheHit);

// 200 картинок 128×128 с нуля. Аргумент — число фоновых потоков.
void BM_Manager_AsyncLoad200(benchmark::State& state) {
    as::VirtualFileSystem vfs;
    auto source = std::make_unique<as::MemorySource>();
    const Bytes png = make_png(128, 128);
    for (int i = 0; i < 200; ++i) source->add("img/" + std::to_string(i) + ".png", png);
    vfs.mount(std::move(source));
    JobSystem::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    for (auto _ : state) {
        as::AssetManager assets(vfs, &jobs);
        std::vector<as::Handle<as::ImageAsset>> handles;
        handles.reserve(200);
        for (int i = 0; i < 200; ++i) handles.push_back(assets.load<as::ImageAsset>("img/" + std::to_string(i) + ".png"));
        assets.wait_all();
        benchmark::DoNotOptimize(handles.back().get());
    }
    state.SetItemsProcessed(state.iterations() * 200);
}
BENCHMARK(BM_Manager_AsyncLoad200)->Arg(0)->Arg(1)->Arg(3)->UseRealTime();

} // namespace

BENCHMARK_MAIN();
