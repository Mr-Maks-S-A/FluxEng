/**
 * @file 03_mods_and_untrusted_data.cpp
 * @brief Моды и данные от сервера: перекрытие файлов, лимиты и враждебные файлы.
 *
 * В мультиплеере вы разбираете то, что прислал сервер или мод. AssetSystem не доверяет данным: размеры проверяются
 * до выделения памяти, а битый файл — это Result с ошибкой, а не падение.
 */

#include <AssetSystem/AssetSystem.hpp>

#include <cstdio>
#include <string_view>

namespace as = AssetSystem;

int main() {
    as::VirtualFileSystem vfs;

    auto base = std::make_unique<as::MemorySource>("base game");
    base->add("skins/mage.bmp", as::procedural::make_bmp(32, 32));
    base->add_text("data/spells.json", R"({"fireball":12})");
    vfs.mount(std::move(base));

    auto mod = std::make_unique<as::MemorySource>("mod: big skins");
    mod->add("skins/mage.bmp", as::procedural::make_bmp(64, 64)); // перекрывает базовый скин
    mod->add_text("skins/evil.bmp", "BM this is not a real bitmap");
    mod->add("skins/huge.bmp", as::procedural::make_bmp(512, 512)); // больше лимита ниже
    vfs.mount(std::move(mod));

    // Лимиты для недоверенного контента: скины не больше 256×256.
    as::AssetManagerConfig config;
    config.limits.max_image_side = 256;
    as::AssetManager assets(vfs, nullptr, config);

    int problems = 0;
    const auto report = [&](std::string_view path) {
        const auto h = assets.load<as::ImageAsset>(path);
        if (h.ready()) {
            std::printf("  %-18.*s ок, %dx%d\n", static_cast<int>(path.size()), path.data(), h.get()->width, h.get()->height);
        } else {
            std::printf("  %-18.*s отказ: %s\n", static_cast<int>(path.size()), path.data(), h.error()->what().c_str());
            ++problems;
        }
        return h;
    };
    const auto mage = report("skins/mage.bmp");   // 64×64 из мода
    report("skins/evil.bmp");                      // битый
    report("skins/huge.bmp");                      // за лимитом
    report("skins/../../etc/passwd");              // путь наружу
    report("skins/none.bmp");                      // нет файла

    return (mage.ready() && mage.get()->width == 64 && problems == 4) ? 0 : 1;
}
