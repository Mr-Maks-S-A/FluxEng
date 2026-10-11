#include "TestAssets.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

using namespace AssetSystem;
using namespace testassets;

namespace {

VirtualFileSystem vfs_with(std::initializer_list<std::pair<const char*, Bytes>> files) {
    auto memory = std::make_unique<MemorySource>();
    for (const auto& [path, data] : files) memory->add(path, data);
    VirtualFileSystem vfs;
    vfs.mount(std::move(memory));
    return vfs;
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/// Каждый треугольник смотрит туда же, куда нормали его вершин, а нормали единичные.
void check_outward_and_unit(const MeshAsset& mesh) {
    for (const MeshVertex& v : mesh.vertices) CHECK(std::sqrt(dot3(v.normal, v.normal)) == doctest::Approx(1.0f).epsilon(1e-4));
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const MeshVertex& a = mesh.vertices[mesh.indices[t]];
        const MeshVertex& b = mesh.vertices[mesh.indices[t + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[t + 2]];
        float e1[3], e2[3];
        for (int k = 0; k < 3; ++k) {
            e1[k] = b.position[k] - a.position[k];
            e2[k] = c.position[k] - a.position[k];
        }
        const float face[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        CHECK(dot3(face, a.normal) > 0.0f);
    }
}

} // namespace

TEST_SUITE("AssetSystem.Image") {

TEST_CASE("PNG декодируется в RGBA8, пиксели на своих местах") {
    const auto image = decode_image(make_png(8, 4));
    REQUIRE(image.has_value());
    CHECK(image->width == 8);
    CHECK(image->height == 4);
    REQUIRE(image->rgba.size() == 8 * 4 * 4);
    const auto px = [&](int x, int y) { return &image->rgba[static_cast<std::size_t>((y * 8 + x) * 4)]; };
    CHECK(px(3, 2)[0] == 3 * 17);
    CHECK(px(3, 2)[1] == 2 * 29);
    CHECK(px(3, 2)[2] == 200);
    CHECK(px(7, 3)[3] == 255);
}

TEST_CASE("BMP сверху вниз (отрицательная высота в заголовке) декодируется, лимиты считаются по модулю") {
    const Bytes bmp = procedural::make_bmp(8, 4);
    const auto image = decode_image(bmp);
    REQUIRE_MESSAGE(image.has_value(), image.error().what());
    CHECK(image->width == 8);
    CHECK(image->height == 4);
    CHECK(image->rgba[3] == 255);
    CHECK(decode_image(bmp, AssetLimits{.max_image_side = 6}).error().code == ErrorCode::TooLarge);
}

TEST_CASE("не картинка, пустые и обрезанные данные — ошибка, а не падение") {
    CHECK(decode_image(bytes_of("this is not an image at all")).error().code == ErrorCode::Decode);
    CHECK(decode_image({}).error().code == ErrorCode::Decode);
    const Bytes png = make_png(16, 16);
    for (std::size_t n = 0; n < png.size(); n += 7) {
        CAPTURE(n);
        (void)decode_image(std::span<const std::byte>(png.data(), n)); // результат любой, главное — без падения
    }
    const auto cut = decode_image(std::span<const std::byte>(png.data(), png.size() / 2));
    CHECK_FALSE(cut.has_value());
}

TEST_CASE("лимиты проверяются по заголовку, до выделения памяти под пиксели") {
    const Bytes png = make_png(32, 8);
    AssetLimits side{.max_image_side = 16};
    CHECK(decode_image(png, side).error().code == ErrorCode::TooLarge);
    AssetLimits pixels{.max_image_pixels = 100};
    CHECK(decode_image(png, pixels).error().code == ErrorCode::TooLarge);
    CHECK(decode_image(png).has_value());
}

TEST_CASE("случайная порча PNG не роняет декодер") {
    const Bytes png = make_png(12, 12);
    std::mt19937 rng(7);
    for (int round = 0; round < 500; ++round) {
        Bytes bytes = png;
        for (int f = 0; f < 3; ++f) bytes[rng() % bytes.size()] = static_cast<std::byte>(rng());
        (void)decode_image(bytes);
    }
}

} // TEST_SUITE

TEST_SUITE("AssetSystem.Gltf") {

TEST_CASE("куб со встроенным буфером: вершины, индексы, границы, нормали, материал") {
    const Gltf g = make_cube_gltf();
    const auto mesh = parse_gltf(bytes_of(g.json), "models/cube.gltf", nullptr);
    REQUIRE_MESSAGE(mesh.has_value(), mesh.error().what());
    CHECK(mesh->vertices.size() == 24);
    CHECK(mesh->indices.size() == 36);
    CHECK(mesh->triangle_count() == 12);
    for (int k = 0; k < 3; ++k) {
        CHECK(mesh->bounds_min[k] == doctest::Approx(-0.5f));
        CHECK(mesh->bounds_max[k] == doctest::Approx(0.5f));
    }
    CHECK(mesh->base_color[1] == doctest::Approx(0.5f));
    CHECK(mesh->base_color_texture == "models/textures/albedo.png"); // путь считается от файла glTF
    check_outward_and_unit(*mesh);
}

TEST_CASE("внешний .bin читается через VFS, а не с диска") {
    const Gltf g = make_cube_gltf({.embedded = false});
    const VirtualFileSystem vfs = vfs_with({{"models/cube.bin", g.bin}});
    const auto mesh = parse_gltf(bytes_of(g.json), "models/cube.gltf", &vfs);
    REQUIRE_MESSAGE(mesh.has_value(), mesh.error().what());
    CHECK(mesh->vertices.size() == 24);

    const VirtualFileSystem empty;
    const auto missing = parse_gltf(bytes_of(g.json), "models/cube.gltf", &empty);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_FALSE(parse_gltf(bytes_of(g.json), "models/cube.gltf", nullptr).has_value());
}

TEST_CASE("GLB: буфер внутри контейнера") {
    const Gltf g = make_cube_gltf({.embedded = false});
    const auto mesh = parse_gltf(make_glb(g.json, g.bin), "cube.glb", nullptr);
    REQUIRE_MESSAGE(mesh.has_value(), mesh.error().what());
    CHECK(mesh->vertices.size() == 24);
    check_outward_and_unit(*mesh);
}

TEST_CASE("преобразование узла применяется к позициям и нормалям") {
    const Gltf g = make_cube_gltf({.translation = {1, 2, 3}, .scale = {2, 2, 2}});
    const auto mesh = parse_gltf(bytes_of(g.json), "c.gltf", nullptr);
    REQUIRE(mesh.has_value());
    const float lo[3] = {0, 1, 2}, hi[3] = {2, 3, 4};
    for (int k = 0; k < 3; ++k) {
        CHECK(mesh->bounds_min[k] == doctest::Approx(lo[k]));
        CHECK(mesh->bounds_max[k] == doctest::Approx(hi[k]));
    }
    check_outward_and_unit(*mesh);
}

TEST_CASE("отражение (отрицательный масштаб) не выворачивает куб: обход и нормали остаются наружу") {
    const Gltf g = make_cube_gltf({.scale = {-1, 1, 1}});
    const auto mesh = parse_gltf(bytes_of(g.json), "c.gltf", nullptr);
    REQUIRE(mesh.has_value());
    check_outward_and_unit(*mesh);
}

TEST_CASE("нет нормалей — достраиваются; нет текстуры — путь пуст") {
    const Gltf g = make_cube_gltf({.with_normals = false, .with_texture = false});
    const auto mesh = parse_gltf(bytes_of(g.json), "c.gltf", nullptr);
    REQUIRE(mesh.has_value());
    CHECK(mesh->base_color_texture.empty());
    check_outward_and_unit(*mesh);
}

TEST_CASE("путь текстуры не может выйти из VFS") {
    const Gltf g = make_cube_gltf({.texture_uri = "../../../etc/passwd"});
    const auto mesh = parse_gltf(bytes_of(g.json), "a/c.gltf", nullptr);
    REQUIRE(mesh.has_value());
    CHECK(mesh->base_color_texture.empty()); // не нормализуется — текстуры нет, а не «чужой файл»
}

TEST_CASE("неподдержанные и испорченные glTF отвергаются с понятным кодом") {
    CHECK(parse_gltf(bytes_of(make_cube_gltf({.primitive_mode = 5}).json), "c.gltf", nullptr).error().code == ErrorCode::Unsupported);
    {
        // Индекс вне диапазона отсекает валидация cgltf (Decode) или наша проверка (Corrupt) — в любом случае это отказ.
        const auto r = parse_gltf(bytes_of(make_cube_gltf({.bad_index = 9999}).json), "c.gltf", nullptr);
        REQUIRE_FALSE(r.has_value());
        CHECK((r.error().code == ErrorCode::Decode || r.error().code == ErrorCode::Corrupt));
    }
    CHECK(parse_gltf(bytes_of("{ this is not json"), "c.gltf", nullptr).error().code == ErrorCode::Decode);
    CHECK(parse_gltf(bytes_of("{}"), "c.gltf", nullptr).error().code == ErrorCode::Decode);
    CHECK(parse_gltf({}, "c.gltf", nullptr).error().code == ErrorCode::Decode);
}

TEST_CASE("лимиты вершин и индексов") {
    const Gltf g = make_cube_gltf();
    AssetLimits few{.max_vertices = 10};
    CHECK(parse_gltf(bytes_of(g.json), "c.gltf", nullptr, few).error().code == ErrorCode::TooLarge);
    AssetLimits tiny_file{.max_file_bytes = 100};
    CHECK(parse_gltf(bytes_of(g.json), "c.gltf", nullptr, tiny_file).error().code == ErrorCode::TooLarge);
}

TEST_CASE("усечённый и случайно испорченный glTF/GLB не роняет разбор") {
    const Gltf g = make_cube_gltf({.embedded = false});
    const Bytes glb = make_glb(g.json, g.bin);
    for (std::size_t n = 0; n < glb.size(); n += 11) (void)parse_gltf(std::span<const std::byte>(glb.data(), n), "c.glb", nullptr);
    std::mt19937 rng(99);
    for (int round = 0; round < 400; ++round) {
        Bytes bytes = glb;
        for (int f = 0; f < 3; ++f) bytes[rng() % bytes.size()] = static_cast<std::byte>(rng());
        (void)parse_gltf(bytes, "c.glb", nullptr);
    }
    const Bytes json = bytes_of(make_cube_gltf().json);
    for (int round = 0; round < 400; ++round) {
        Bytes bytes = json;
        bytes[rng() % bytes.size()] = static_cast<std::byte>(rng());
        (void)parse_gltf(bytes, "c.gltf", nullptr);
    }
}

} // TEST_SUITE

TEST_SUITE("AssetSystem.MeshBinary") {

MeshAsset cube() { return *parse_gltf(bytes_of(make_cube_gltf().json), "models/cube.gltf", nullptr); }

TEST_CASE("serialize → parse: сетка совпадает до байта") {
    const MeshAsset original = cube();
    const Bytes data = serialize_mesh(original);
    const auto parsed = parse_mesh_binary(data);
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error().what());
    CHECK(parsed->vertices.size() == original.vertices.size());
    CHECK(std::memcmp(parsed->vertices.data(), original.vertices.data(), original.vertices.size() * sizeof(MeshVertex)) == 0);
    CHECK(parsed->indices == original.indices);
    CHECK(parsed->base_color_texture == original.base_color_texture);
    CHECK(parsed->bounds_min[0] == doctest::Approx(-0.5f));
    CHECK(serialize_mesh(*parsed) == data); // стабильный формат
}

TEST_CASE("пустая сетка тоже круглая") {
    MeshAsset empty;
    const auto parsed = parse_mesh_binary(serialize_mesh(empty));
    REQUIRE(parsed.has_value());
    CHECK(parsed->vertices.empty());
}

TEST_CASE("любое усечение отвергается") {
    const Bytes data = serialize_mesh(cube());
    for (std::size_t n = 0; n < data.size(); ++n) {
        CAPTURE(n);
        CHECK_FALSE(parse_mesh_binary(std::span<const std::byte>(data.data(), n)).has_value());
    }
}

TEST_CASE("порча любого одного байта ловится (CRC-32 находит все однобайтовые ошибки)") {
    const Bytes data = serialize_mesh(cube());
    for (std::size_t i = 0; i < data.size(); ++i) {
        CAPTURE(i);
        Bytes bad = data;
        bad[i] ^= std::byte{0x5A};
        CHECK_FALSE(parse_mesh_binary(bad).has_value());
    }
}

TEST_CASE("индекс вне диапазона даже с верной суммой — Corrupt; лимиты — TooLarge") {
    MeshAsset broken = cube();
    broken.indices[0] = 1000;
    CHECK(parse_mesh_binary(serialize_mesh(broken)).error().code == ErrorCode::Corrupt);
    CHECK(parse_mesh_binary(serialize_mesh(cube()), AssetLimits{.max_vertices = 4}).error().code == ErrorCode::TooLarge);
    broken = cube();
    broken.indices.pop_back(); // не кратно трём
    CHECK(parse_mesh_binary(serialize_mesh(broken)).error().code == ErrorCode::Corrupt);
}

TEST_CASE("cook_directory: glTF → .fmesh, PNG и текст копируются, .bin пропускается, результат воспроизводим") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "flux_asset_test_cook";
    fs::remove_all(dir);
    fs::create_directories(dir / "models");
    fs::create_directories(dir / "textures");
    const Gltf g = make_cube_gltf({.embedded = false});
    const auto write = [&](const fs::path& p, const Bytes& b) {
        std::ofstream(dir / p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    };
    write("models/cube.gltf", bytes_of(g.json));
    write("models/cube.bin", g.bin);
    write("textures/albedo.png", make_png(8, 8));
    write("spells.json", bytes_of(R"({"a":1})"));

    PackWriter first, second;
    const auto report = cook_directory(dir, first);
    REQUIRE_MESSAGE(report.has_value(), report.error().what());
    CHECK(report->files == 3);
    CHECK(report->meshes_cooked == 1);
    CHECK(report->warnings.size() == 1); // .bin пропущен
    REQUIRE(cook_directory(dir, second).has_value());
    CHECK(*first.build() == *second.build());

    auto pack = PackSource::open(*first.build());
    REQUIRE(pack.has_value());
    CHECK((*pack)->exists("models/cube.fmesh"));
    CHECK_FALSE((*pack)->exists("models/cube.gltf"));
    CHECK_FALSE((*pack)->exists("models/cube.bin"));
    const auto cooked = parse_mesh_binary(*(*pack)->read("models/cube.fmesh"));
    REQUIRE(cooked.has_value());
    CHECK(cooked->vertices.size() == 24);

    write("models/broken.gltf", bytes_of("{ not json"));
    PackWriter third;
    const auto failed = cook_directory(dir, third);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message.find("broken.gltf") != std::string::npos); // ошибка называет файл
    fs::remove_all(dir);
}

} // TEST_SUITE
