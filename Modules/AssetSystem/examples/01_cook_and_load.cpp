/**
 * @file 01_cook_and_load.cpp
 * @brief Путь ассета от исходника до игры: glTF на диске → cook → пак → VFS → менеджер.
 *
 * 1. Исходники (glTF-куб с внешним .bin, текстура BMP, JSON заклинаний) записываются во временный каталог;
 * 2. cook_directory превращает glTF в бинарный `.fmesh` и собирает пак;
 * 3. рантайм монтирует пак и грузит ассеты менеджером — никакого JSON и base64 при запуске игры.
 */

#include <AssetSystem/AssetSystem.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace as = AssetSystem;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, const as::Bytes& data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main() {
    const fs::path source = fs::temp_directory_path() / "flux_example_assets_src";
    const fs::path pack_file = fs::temp_directory_path() / "flux_example_assets.fxpk";
    fs::remove_all(source);

    // --- 1. исходники
    const auto cube = as::procedural::make_gltf_box({.embedded = false, .texture_uri = "../textures/stone.bmp"});
    write(source / "models/cube.gltf", {reinterpret_cast<const std::byte*>(cube.json.data()), reinterpret_cast<const std::byte*>(cube.json.data()) + cube.json.size()});
    write(source / "models/cube.bin", cube.bin);
    write(source / "textures/stone.bmp", as::procedural::make_bmp(64, 64));
    const std::string spells = R"({"fireball":{"damage":12,"mana":8}})";
    write(source / "data/spells.json", {reinterpret_cast<const std::byte*>(spells.data()), reinterpret_cast<const std::byte*>(spells.data()) + spells.size()});

    // --- 2. cook
    as::PackWriter pack;
    const auto report = as::cook_directory(source, pack);
    if (!report) {
        std::fprintf(stderr, "cook: %s\n", report.error().what().c_str());
        return 1;
    }
    if (const auto written = pack.write_file(pack_file); !written) {
        std::fprintf(stderr, "write: %s\n", written.error().what().c_str());
        return 1;
    }
    std::printf("cook: %zu файлов (%zu сеток), %zu -> %zu байт; предупреждений: %zu\n", report->files, report->meshes_cooked,
                report->bytes_in, report->bytes_out, report->warnings.size());

    // --- 3. рантайм: пак поверх всего остального
    as::VirtualFileSystem vfs;
    auto mounted = as::PackSource::open_file(pack_file);
    if (!mounted) {
        std::fprintf(stderr, "pack: %s\n", mounted.error().what().c_str());
        return 1;
    }
    vfs.mount(std::move(*mounted));
    for (const std::string& path : vfs.list()) std::printf("  в паке: %s\n", path.c_str());

    as::AssetManager assets(vfs); // без планировщика: загрузка синхронная
    const auto start = std::chrono::steady_clock::now();
    const auto mesh = assets.load_sync<as::MeshAsset>("models/cube.fmesh");
    const auto texture = assets.load_sync<as::ImageAsset>("textures/stone.bmp");
    const auto data = assets.load_sync<as::TextAsset>("data/spells.json");
    if (!mesh || !texture || !data) {
        std::fprintf(stderr, "load failed\n");
        return 1;
    }
    std::printf("загружено за %.3f мс: сетка %zu вершин / %zu треугольников, текстура %dx%d, данные %zu байт\n",
                milliseconds_since(start), (*mesh)->vertices.size(), (*mesh)->triangle_count(), (*texture)->width, (*texture)->height,
                (*data)->text.size());
    std::printf("путь текстуры из сетки: %s\n", (*mesh)->base_color_texture.c_str());

    fs::remove_all(source);
    fs::remove(pack_file);
    return (*mesh)->base_color_texture == "textures/stone.bmp" ? 0 : 1;
}
