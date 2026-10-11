#include "TestAssets.hpp"

#include <JobSystem/JobSystem.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

using namespace AssetSystem;
using namespace testassets;

namespace {

struct Fixture {
    VirtualFileSystem vfs;
    MemorySource* memory = nullptr;
    Fixture() {
        auto source = std::make_unique<MemorySource>();
        memory = source.get(); // тест добавляет файлы до начала загрузок
        memory->add("img/a.png", make_png(8, 8));
        memory->add("img/b.png", make_png(16, 4));
        memory->add_text("data/spells.json", R"({"fireball":12})");
        memory->add("models/cube.fmesh", serialize_mesh(*parse_gltf(bytes_of(make_cube_gltf().json), "m/cube.gltf", nullptr)));
        memory->add("models/cube.gltf", bytes_of(make_cube_gltf({.embedded = false}).json));
        memory->add("models/cube.bin", make_cube_gltf({.embedded = false}).bin);
        memory->add_text("bad/broken.png", "definitely not a png");
        vfs.mount(std::move(source));
    }
};

/// Тип для проверки «загрузчик вызывается один раз».
struct Counted {
    int value = 0;
    [[nodiscard]] std::size_t size_bytes() const noexcept { return sizeof(int); }
};

} // namespace

TEST_SUITE("AssetSystem.Manager") {

TEST_CASE("синхронная загрузка без планировщика: готово сразу после load()") {
    Fixture f;
    AssetManager assets(f.vfs);
    const auto image = assets.load<ImageAsset>("img/a.png");
    REQUIRE(image.ready());
    CHECK(image.get()->width == 8);
    CHECK(image.path() == "img/a.png");
    CHECK(assets.load_sync<TextAsset>("data/spells.json").value()->text == R"({"fireball":12})");
    const auto mesh = assets.load_sync<MeshAsset>("models/cube.gltf"); // внешний .bin — из VFS
    REQUIRE_MESSAGE(mesh.has_value(), mesh.error().what());
    CHECK((*mesh)->vertices.size() == 24);
    CHECK(assets.load_sync<MeshAsset>("models/cube.fmesh").value()->indices.size() == 36);
}

TEST_CASE("кеш: тот же путь — тот же слот; разные написания пути — тоже") {
    Fixture f;
    AssetManager assets(f.vfs);
    const auto a = assets.load<ImageAsset>("img/a.png");
    const auto again = assets.load<ImageAsset>("./img//a.png");
    CHECK(a == again);
    CHECK(a.get() == again.get());
    CHECK(assets.stats().loads_started == 1);
    CHECK(assets.stats().cache_hits == 1);
    CHECK_FALSE(a == assets.load<ImageAsset>("img/b.png"));
}

TEST_CASE("ошибки — в Handle, а не исключения") {
    Fixture f;
    AssetManager assets(f.vfs);
    const auto missing = assets.load<ImageAsset>("img/none.png");
    REQUIRE(missing.failed());
    CHECK(missing.error()->code == ErrorCode::NotFound);
    CHECK(missing.get() == nullptr);
    CHECK(assets.load<ImageAsset>("bad/broken.png").error()->code == ErrorCode::Decode);
    CHECK(assets.load<ImageAsset>("../outside.png").error()->code == ErrorCode::InvalidPath);
    CHECK(assets.load<ImageAsset>("img/a.unknown").error()->code == ErrorCode::Unsupported);
    CHECK(assets.load<TextAsset>("img/a.png").error()->code == ErrorCode::Unsupported); // для PNG нет загрузчика текста
    CHECK_FALSE(assets.load_sync<ImageAsset>("img/none.png").has_value());
    CHECK_FALSE(Handle<ImageAsset>{}.valid());
    CHECK(Handle<ImageAsset>{}.failed());
}

TEST_CASE("исключение из загрузчика становится ошибкой Decode") {
    Fixture f;
    AssetManager assets(f.vfs);
    assets.register_loader<Counted>(".cnt", [](const LoadContext&) -> Result<Counted> { throw std::runtime_error("boom"); });
    f.memory->add_text("x.cnt", "1");
    const auto h = assets.load<Counted>("x.cnt");
    REQUIRE(h.failed());
    CHECK(h.error()->code == ErrorCode::Decode);
    CHECK(h.error()->message.find("boom") != std::string::npos);
}

TEST_CASE("один путь, два типа — два независимых ассета") {
    Fixture f;
    AssetManager assets(f.vfs);
    assets.register_loader<TextAsset>(".dat", [](const LoadContext& c) -> Result<TextAsset> {
        return TextAsset{std::string(reinterpret_cast<const char*>(c.bytes.data()), c.bytes.size())};
    });
    f.memory->add_text("raw.dat", "abc");
    const auto text = assets.load<TextAsset>("raw.dat");
    const auto blob = assets.load<BlobAsset>("raw.dat");
    REQUIRE(text.ready());
    REQUIRE(blob.ready());
    CHECK(text.get()->text == "abc");
    CHECK(blob.get()->bytes.size() == 3);
}

TEST_CASE("асинхронная загрузка при 0, 1 и 3 фоновых потоках: все готовы, результат один и тот же") {
    for (const unsigned threads : {0u, 1u, 3u}) {
        CAPTURE(threads);
        Fixture f;
        for (int i = 0; i < 40; ++i) f.memory->add("many/" + std::to_string(i) + ".png", make_png(4 + i % 5, 4));
        JobSystem::Scheduler jobs({.threads = threads});
        AssetManager assets(f.vfs, &jobs);

        std::vector<Handle<ImageAsset>> handles;
        for (int i = 0; i < 40; ++i) handles.push_back(assets.load<ImageAsset>("many/" + std::to_string(i) + ".png"));
        assets.wait_all();
        for (int i = 0; i < 40; ++i) {
            REQUIRE(handles[static_cast<std::size_t>(i)].ready());
            CHECK(handles[static_cast<std::size_t>(i)].get()->width == 4 + i % 5);
        }
        const AssetStats stats = assets.stats();
        CHECK(stats.loads_ok == 40);
        CHECK(stats.loads_failed == 0);
        CHECK(stats.in_flight == 0);
    }
}

TEST_CASE("on_done: обратный вызов идёт только внутри pump() и в вызывающем потоке") {
    Fixture f;
    JobSystem::Scheduler jobs({.threads = 3});
    AssetManager assets(f.vfs, &jobs);

    std::vector<std::thread::id> callers;
    int ok = 0, failed = 0;
    auto record = [&](const auto& h) {
        callers.push_back(std::this_thread::get_id());
        (h.ready() ? ok : failed) += 1;
    };
    const auto good = assets.load<ImageAsset>("img/a.png");
    const auto bad = assets.load<ImageAsset>("img/none.png");
    assets.on_done<ImageAsset>(good, record);
    assets.on_done<ImageAsset>(bad, record);
    assets.wait_all();
    CHECK(callers.empty()); // загрузка закончилась, но вызовов ещё нет: они ждут pump()

    CHECK(assets.pump() == 2);
    CHECK(ok == 1);
    CHECK(failed == 1);
    for (const auto& id : callers) CHECK(id == std::this_thread::get_id());

    assets.on_done<ImageAsset>(good, record); // регистрация после готовности — на ближайший pump()
    CHECK(callers.size() == 2);
    CHECK(assets.pump() == 1);
    CHECK(ok == 2);
    CHECK(assets.pump() == 0);
}

TEST_CASE("загрузчик вызывается один раз, даже если просят 8 потоков одновременно") {
    Fixture f;
    f.memory->add_text("once.cnt", "7");
    JobSystem::Scheduler jobs({.threads = 3});
    AssetManager assets(f.vfs, &jobs);
    std::atomic<int> calls{0};
    assets.register_loader<Counted>(".cnt", [&](const LoadContext&) -> Result<Counted> {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return Counted{7};
    });

    std::vector<Handle<Counted>> handles(8);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        threads.emplace_back([&, i] { handles[i] = assets.load<Counted>("once.cnt"); });
    }
    for (auto& t : threads) t.join();
    for (const auto& h : handles) CHECK(h == handles[0]);
    REQUIRE(assets.wait(handles[0]).has_value());
    CHECK(calls == 1);
    CHECK(handles[3].get()->value == 7);
}

TEST_CASE("collect(): записи без владельцев забываются, живые остаются; evict → перечитать файл") {
    Fixture f;
    AssetManager assets(f.vfs);
    auto keep = assets.load<ImageAsset>("img/a.png");
    { auto temporary = assets.load<ImageAsset>("img/b.png"); }
    CHECK(assets.collect() == 1);
    CHECK(assets.load<ImageAsset>("img/a.png") == keep);

    f.memory->add("img/a.png", make_png(2, 2)); // «файл изменился»
    CHECK(assets.load<ImageAsset>("img/a.png").get()->width == 8); // кеш всё ещё отдаёт старое
    CHECK(assets.evict<ImageAsset>("img/a.png"));
    CHECK(assets.load<ImageAsset>("img/a.png").get()->width == 2);
    CHECK(keep.get()->width == 8); // у прежних владельцев ассет не пропал
}

TEST_CASE("Handle переживает менеджер; менеджер дожидается задач в полёте при уничтожении") {
    Fixture f;
    for (int i = 0; i < 30; ++i) f.memory->add("slow/" + std::to_string(i) + ".png", make_png(8, 8));
    JobSystem::Scheduler jobs({.threads = 3});
    Handle<ImageAsset> survivor;
    {
        AssetManager assets(f.vfs, &jobs);
        survivor = assets.load<ImageAsset>("img/a.png");
        for (int i = 0; i < 30; ++i) (void)assets.load<ImageAsset>("slow/" + std::to_string(i) + ".png");
        (void)assets.wait(survivor);
    } // деструктор: задачи ещё могут идти
    CHECK(survivor.ready());
    CHECK(survivor.get()->height == 8);
}

TEST_CASE("загрузка из пака, смонтированного поверх каталога") {
    PackWriter writer;
    writer.add("img/a.png", make_png(2, 2));
    VirtualFileSystem vfs;
    auto base = std::make_unique<MemorySource>();
    base->add("img/a.png", make_png(8, 8));
    base->add("img/b.png", make_png(5, 5));
    vfs.mount(std::move(base));
    vfs.mount(std::move(*PackSource::open(*writer.build())));
    AssetManager assets(vfs);
    CHECK(assets.load<ImageAsset>("img/a.png").get()->width == 2); // перекрыто паком
    CHECK(assets.load<ImageAsset>("img/b.png").get()->width == 5);
}

} // TEST_SUITE
