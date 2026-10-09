#include "TestAssets.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace AssetSystem;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("flux_asset_test_" + name);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    void write(const fs::path& relative, std::string_view text) const {
        fs::create_directories((path / relative).parent_path());
        std::ofstream(path / relative, std::ios::binary) << text;
    }
};

std::string text_of(const Bytes& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

} // namespace

TEST_SUITE("AssetSystem.Paths") {

TEST_CASE("normalize_path: слэши, точки, повторы") {
    CHECK(*normalize_path("models/staff.fmesh") == "models/staff.fmesh");
    CHECK(*normalize_path("models\\staff.fmesh") == "models/staff.fmesh");
    CHECK(*normalize_path("./models//./staff.fmesh") == "models/staff.fmesh");
    CHECK(*normalize_path("/models/staff.fmesh") == "models/staff.fmesh");
    CHECK(*normalize_path("a/b/../c.png") == "a/c.png");
    CHECK(*normalize_path("a/b/../../c.png") == "c.png");
}

TEST_CASE("normalize_path: отказ для выхода за корень, диска, пустого пути и управляющих символов") {
    for (const char* bad : {"../x", "a/../../x", "", "/", ".", "C:/x", "a:b", "a/\x01/b", "a\nb"}) {
        CAPTURE(bad);
        const auto r = normalize_path(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidPath);
    }
}

TEST_CASE("AssetId: стабильное значение (золотой тест) и одинаковый id для разных написаний") {
    CHECK(asset_id("models/staff.fmesh")->value == 0x1f33f236c1e125b7ull); // FNV-1a 64: не должен меняться между версиями
    CHECK(*asset_id("models\\staff.fmesh") == *asset_id("./models//staff.fmesh"));
    CHECK(*asset_id("a.png") != *asset_id("b.png"));
    CHECK_FALSE(asset_id("../a.png").has_value());
}

TEST_CASE("extension_of и directory_of") {
    CHECK(extension_of("a/b/Tex.PNG") == ".png");
    CHECK(extension_of("a/b/noext") == "");
    CHECK(extension_of("a/.hidden") == "");
    CHECK(extension_of("a.b/file") == "");
    CHECK(directory_of("a/b/c.png") == "a/b");
    CHECK(directory_of("c.png") == "");
}

} // TEST_SUITE

TEST_SUITE("AssetSystem.Vfs") {

TEST_CASE("MemorySource читает по нормализованному пути") {
    MemorySource memory;
    CHECK(memory.add_text("dir\\a.txt", "hello"));
    CHECK_FALSE(memory.add_text("../escape.txt", "x"));
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<MemorySource>(std::move(memory)));
    CHECK(text_of(*vfs.read("./dir//a.txt")) == "hello");
    CHECK(vfs.exists("dir/a.txt"));
    CHECK_FALSE(vfs.exists("dir/b.txt"));
    const auto missing = vfs.read("dir/b.txt");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("поздно смонтированный источник перекрывает ранний, остальные файлы остаются видны") {
    auto base = std::make_unique<MemorySource>("base");
    base->add_text("a.txt", "base-a");
    base->add_text("b.txt", "base-b");
    auto mod = std::make_unique<MemorySource>("mod");
    mod->add_text("a.txt", "mod-a");
    VirtualFileSystem vfs;
    vfs.mount(std::move(base));
    vfs.mount(std::move(mod));
    CHECK(text_of(*vfs.read("a.txt")) == "mod-a");
    CHECK(text_of(*vfs.read("b.txt")) == "base-b");
    CHECK(vfs.list() == std::vector<std::string>{"a.txt", "b.txt"}); // без повторов, по возрастанию
    CHECK(vfs.mount_count() == 2);
}

TEST_CASE("DirectorySource: чтение, список, лимит размера") {
    TempDir dir("dir_basic");
    dir.write("sub/a.txt", "alpha");
    dir.write("big.dat", std::string(1000, 'x'));
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectorySource>(dir.path, 100));
    CHECK(text_of(*vfs.read("sub/a.txt")) == "alpha");
    const auto big = vfs.read("big.dat");
    REQUIRE_FALSE(big.has_value());
    CHECK(big.error().code == ErrorCode::TooLarge);
    CHECK(vfs.list() == std::vector<std::string>{"big.dat", "sub/a.txt"});
}

TEST_CASE("DirectorySource: ни `..`, ни симлинк не выводят за корень") {
    TempDir dir("dir_escape");
    dir.write("root/inside.txt", "inside");
    dir.write("secret.txt", "secret");
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectorySource>(dir.path / "root"));

    CHECK(vfs.exists("inside.txt"));
    CHECK_FALSE(vfs.read("../secret.txt").has_value()); // отсекает normalize_path
    CHECK_FALSE(vfs.exists("../secret.txt"));

    std::error_code ec;
    fs::create_symlink(dir.path / "secret.txt", dir.path / "root" / "link.txt", ec);
    if (ec) {
        MESSAGE("symlinks are unavailable here — symlink check skipped");
    } else {
        CHECK_FALSE(vfs.exists("link.txt"));
        CHECK_FALSE(vfs.read("link.txt").has_value());
        const DirectorySource direct(dir.path / "root");
        const auto through_link = direct.read("link.txt"); // сам источник называет причину
        REQUIRE_FALSE(through_link.has_value());
        CHECK(through_link.error().code == ErrorCode::InvalidPath);
    }
}

} // TEST_SUITE
