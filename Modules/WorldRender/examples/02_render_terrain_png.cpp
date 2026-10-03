/**
 * @example 02_render_terrain_png.cpp
 * Полный кадр сцены в PNG без видимого окна: ландшафт SDF, туман маны с «дырой» после заклинания, отладочные линии.
 *
 * Показан весь путь: источники (адаптеры Terrain и ManaField) → очередь сеток → TerrainView / FogView → картинка.
 * Результат: `worldrender_scene.png` в текущем каталоге (путь можно передать аргументом).
 */

#include <WorldRender/Adapters/Terrain.hpp>
#include <WorldRender/WorldRender.hpp>

#include <WindowSystem/Window.hpp>

#include <cstdio>
#include <set>
#include <string>

using namespace WorldRender;
using namespace RendererSystem;

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "worldrender_scene.png";
    constexpr int width = 640, height = 360;

    // Окно нужно только ради контекста OpenGL, поэтому оно скрыто. Окно объявлено первым — устройство умирает раньше.
    auto window = WindowSystem::Window::create({.title = "worldrender", .width = 64, .height = 64, .visible = false, .vsync = false});
    if (!window) {
        std::fprintf(stderr, "окно: %s\n", window.error().c_str());
        return 1;
    }
    auto device = RHI::Device::create({.backend = Backend::OpenGL});
    if (!device) {
        std::fprintf(stderr, "устройство: %s\n", device.error().c_str());
        return 1;
    }
    RHI::Device& dev = **device;

    // 1. Мир и поле маны; «заклинание» вырезает яму и забирает ману из тумана.
    Terrain::SdfWorld world(1);
    ManaField::ManaGrid mana;
    const std::int64_t gx = 64 * 65536, gz = 64 * 65536;
    const Math::WorldPos centre{gx, world.ground_height(gx, gz), gz};
    (void)world.carve_sphere(centre, Math::Fixed::from_int(3));
    (void)mana.draw({centre.x, centre.y + 3 * 65536, centre.z}, Math::Fixed::from_int(3), Math::Mana::from_int(240));

    // 2. Источники данных для рендера: рендер видит только интерфейсы SurfaceSource / FogSource.
    const TerrainSurface surface(world);
    const ManaFogSource fog_source(mana, world);

    // 3. Очередь сеток (всё сразу — это стартовый мир), построение параллельно, загрузка на GPU.
    JobSystem::Scheduler jobs({.threads = 2});
    TerrainView terrain_view(dev, surface);
    terrain_view.enqueue(TerrainSurface::indices(world.take_dirty()));
    const glm::dvec3 eye_target{64.0, static_cast<double>(centre.y) / 65536.0 + 1.6, 64.0};
    terrain_view.update(jobs, eye_target, /*budget=*/0);
    std::printf("построено %u сеток, %zu треугольников\n", terrain_view.stats().built, terrain_view.stats().triangles);

    FogView fog_view(dev);
    LineRenderer lines(dev);
    DebugDraw debug;
    debug.cross(eye_target, 0.5, Colors::yellow);

    // 4. Камера смотрит на яму со стороны. Всё рисуется относительно глаза (View::relative).
    CameraRig rig;
    rig.yaw = -2.2f;
    rig.pitch = -0.35f;
    rig.distance = 9.0f;
    const View view = rig.view(eye_target, world, {static_cast<float>(width), static_cast<float>(height)});

    // 5. Кадр: ландшафт → туман (после непрозрачного) → линии. Цель с буфером глубины.
    auto frame = RenderTarget::create(dev, width, height, {.depth = true});
    if (!frame) {
        std::fprintf(stderr, "цель: %s\n", frame.error().c_str());
        return 1;
    }
    dev.begin_frame(width, height);
    frame->bind();
    const Color sky = Color::from_rgba(0x8CBDF2FF);
    dev.clear(sky, true);
    terrain_view.draw(view, Light{});
    fog_view.draw(fog_source, view);
    lines.flush(debug, view);
    const Image pixels = frame->read_pixels();
    dev.end_frame();

    // 6. Проверка: картинка не пустая — есть и небо, и земля, и разные цвета.
    std::set<std::uint32_t> colours;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            const Color c = pixels.pixel(x, y);
            colours.insert(static_cast<std::uint32_t>(c.r) << 16 | static_cast<std::uint32_t>(c.g) << 8 | c.b);
        }
    }
    const Color corner = pixels.pixel(2, 2);
    std::printf("различных цветов: %zu, угол картинки: %d %d %d\n", colours.size(), corner.r, corner.g, corner.b);
    if (const auto saved = pixels.save_png(output); !saved) {
        std::fprintf(stderr, "%s\n", saved.error().c_str());
        return 1;
    }
    std::printf("сохранено %s\n", output.c_str());
    if (colours.size() < 100 || terrain_view.stats().drawn == 0) {
        std::printf("ОШИБКА: картинка выглядит пустой\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
