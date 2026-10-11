/**
 * @file 02_async_loading.cpp
 * @brief Фоновая загрузка на JobSystem и обратные вызовы в главном потоке.
 *
 * Игровой цикл не ждёт диск: load() возвращается сразу, разбор идёт в рабочих потоках, а результат
 * забирается в кадре через pump() — именно там безопасно создавать GPU-ресурсы.
 */

#include <AssetSystem/AssetSystem.hpp>

#include <cstdio>
#include <thread>

namespace as = AssetSystem;

int main() {
    // Файлы «на диске» — в памяти, чтобы пример не зависел от каталога.
    as::VirtualFileSystem vfs;
    auto files = std::make_unique<as::MemorySource>();
    for (int i = 0; i < 16; ++i) files->add("tex/t" + std::to_string(i) + ".bmp", as::procedural::make_bmp(128, 128));
    vfs.mount(std::move(files));

    JobSystem::Scheduler jobs({.threads = 3});
    as::AssetManager assets(vfs, &jobs);

    int uploaded = 0;
    std::vector<as::Handle<as::ImageAsset>> handles;
    for (int i = 0; i < 17; ++i) { // последний путь не существует — ошибка придёт тем же путём
        auto handle = assets.load<as::ImageAsset>("tex/t" + std::to_string(i) + ".bmp");
        assets.on_done<as::ImageAsset>(handle, [&uploaded](const as::Handle<as::ImageAsset>& h) {
            if (h.ready()) {
                ++uploaded; // здесь, в главном потоке, можно создавать текстуру GPU из *h.get()
            } else {
                std::printf("  не загрузилось: %s (%s)\n", h.path().c_str(), h.error()->what().c_str());
            }
        });
        handles.push_back(std::move(handle));
    }

    int frames = 0;
    while (uploaded < 16 && frames < 100000) {
        ++frames;
        assets.pump(); // «кадр»: забираем готовое
        std::this_thread::yield();
    }
    assets.wait_all();
    assets.pump();

    const as::AssetStats stats = assets.stats();
    std::printf("кадров ожидания: %d, загружено %llu, ошибок %llu, прочитано %llu байт\n", frames,
                static_cast<unsigned long long>(stats.loads_ok), static_cast<unsigned long long>(stats.loads_failed),
                static_cast<unsigned long long>(stats.bytes_read));
    return uploaded == 16 && stats.loads_failed == 1 ? 0 : 1;
}
