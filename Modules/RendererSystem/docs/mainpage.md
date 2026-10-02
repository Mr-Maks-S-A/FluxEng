# RendererSystem — рендер FluxEng (2D и 3D, OpenGL и Vulkan) {#mainpage}

Рендер поверх **RHI** — тонкого слоя над графическим API с двумя бэкендами: **OpenGL 3.3 core** (glad)
и **Vulkan 1.3** (dynamic rendering, шейдеры компилирует shaderc во время работы). Выше RHI —
ни одного вызова API: Renderer2D, Renderer3D и игры работают на обоих бэкендах без изменений. **2D:** батчевые спрайты, спрайт-листы, анимации, прямоугольники, линии, текст.
**3D:** сетки с материалами, освещение (солнце, рассеянный и до 8 точечных источников), туман,
прозрачность, отсечение по пирамиде видимости, выбор объектов лучом. Общее: камеры, рендер в текстуру,
шрифты, процедурные изображения, PNG.

Подключение: `#include <RendererSystem/RendererSystem.hpp>`, CMake-цель `engine::RendererSystem`.
Зависимости библиотеки: `glm`, `glad`, `stb` (stb_image, stb_image_write, stb_truetype, stb_rect_pack,
stb_easy_font, stb_perlin). Окно и GLFW ей не нужны — достаточно текущего GL-контекста.

## RHI: выбор графического API

```cpp
// OpenGL: окно WindowSystem с контекстом (ClientApi::OpenGL), затем
auto gl = RHI::Device::create({.backend = Backend::OpenGL});
// Vulkan: окно без контекста (ClientApi::None) — или вообще без окна (рендер только в цели)
auto vk = RHI::Device::create({.backend = Backend::Vulkan, .validation = true,
                               .instance_extensions = WindowSystem::Window::vulkan_instance_extensions(),
                               .create_surface = [&](std::uintptr_t i) { return window.create_vulkan_surface(i); }});
auto renderer = Renderer2D::create(*device);   // дальше — тот же код для обоих
```

- **Ручки** (`RHI::BufferId`, `TextureId`, `TargetId`, `PipelineId`) — числа, нулевая — «нет» (ZII);
  владеют ресурсами RAII-обёртки `Texture`, `RenderTarget`, `Mesh`, `Pipeline` (RHI/Resources.hpp).
- **Один GLSL на оба бэкенда**: `#version` и макросы добавляет бэкенд — `FLUX_LOCATION(n)`, `FLUX_VARYING(n)`,
  `FLUX_UNIFORM(set, binding) Frame {…} frame;` (set 0 — блок кадра, set 1 — блок вызова), `FLUX_SAMPLER(2, 0)`,
  `FLUX_POSITION(clip)` (Vulkan переводит глубину −1…1 → 0…1). Ось Y одинакова (у Vulkan — отрицательная высота viewport).
- **Блоки uniform** — `device.push_uniform(data)` кладёт их в память кадра; срез передаётся в `DrawCall::frame`,
  данные вызова — `DrawCall::draw_uniforms`.
- **Текстура цели** рисуется с `RenderTarget::uv()`: в OpenGL строка 0 текстуры — низ, в Vulkan — верх.
- Как устроен Vulkan-бэкенд: один кадр в полёте, кольцо памяти кадра, «буфер, уже нарисованный в этом кадре,
  при обновлении заменяется» (аналог orphaning), ленивые проходы (очистка → loadOp = CLEAR), отложенное удаление.
  Ошибки слоёв валидации считает `Device::validation_messages()` — тесты требуют ноль.
- Проверка равенства бэкендов: тесты рисуют один кадр на OpenGL и Vulkan и сравнивают пиксели.

## Два слоя

| Слой | Что внутри | Нужен GPU |
|---|---|---|
| **CPU-ядро** | RendererSystem::Color, RendererSystem::Rect, RendererSystem::Image, RendererSystem::Camera2D, RendererSystem::SpriteBatch, анимации; RendererSystem::Camera3D, RendererSystem::Ray / RendererSystem::Aabb / RendererSystem::Frustum, RendererSystem::MeshData; RendererSystem::Font; RendererSystem::Procedural | нет |
| **GPU** | RendererSystem::RHI::Device (OpenGL / Vulkan), RendererSystem::Texture, RendererSystem::RenderTarget, RendererSystem::Mesh, RendererSystem::Pipeline, RendererSystem::Renderer2D, RendererSystem::Renderer3D | да |

CPU-ядро превращает кадр в массив вершин и список команд RendererSystem::DrawCommand.
Бэкенд только загружает это в GPU. Поэтому логика кадра тестируется и профилируется
без видеокарты, а другой бэкенд (Vulkan, WebGPU) сможет взять тот же результат.

## Соглашения

- **Ось Y вниз**, (0, 0) — левый верхний угол. Так же устроены пиксели изображений и спрайт-листов.
- **UV (0, 0) — левый верхний пиксель изображения.** Кадр `N` спрайт-листа — это
  `make_grid_frames(grid, N, 1, duration)`, строка 0 — верх листа.
- Углы в радианах; при оси Y вниз положительный угол поворачивает по часовой.
- Цвет — 4 байта RGBA8 (RendererSystem::Color); в вершине он занимает 4 байта, а не 16.
- Текстуры по умолчанию с фильтрацией `Nearest` — для пиксель-арта и воксельных атласов.

## Кадр

```cpp
auto renderer = RendererSystem::Renderer2D::create().value();
TextureHandle goblin = renderer.load_texture("goblin.png").value();

Camera2D camera{.position = player, .zoom = 2.0f, .viewport = {width, height}};

renderer.set_viewport(width, height);
renderer.clear(Colors::black);
renderer.begin(camera);
renderer.draw(SpriteInstance{.position = {10, 20}, .size = {16, 16}, .texture = goblin, .layer = 1});
renderer.fill_rect({{0, 0}, {100, 4}}, Colors::red, 10);
RenderStats stats = renderer.end(); // quads, draw_calls, texture_binds
```

## Батчинг и порядок

RendererSystem::SpriteBatch сортирует спрайты по слою и рисует каждый непрерывный
участок с одной текстурой одним `glDrawElements`. Сортировка поразрядная (LSD radix)
и пропускает разряды, где у всех спрайтов одинаковый байт, поэтому в типичном кадре
это 1–2 линейных прохода.

| RendererSystem::SortMode | Внутри слоя | Когда |
|---|---|---|
| `LayerThenTexture` | группировка по текстуре — минимум draw call'ов | тайлы, воксели, существа, частицы |
| `LayerThenSubmission` | порядок вызовов `draw()` | UI, текст, всё, где важно точное перекрытие |

Практическое правило: чем меньше разных текстур, тем меньше draw call'ов. Собирайте
спрайты в атласы и выбирайте кадр через `uv`.

## Текст

RendererSystem::Font — атлас глифов без OpenGL: TTF/OTF запекается stb_truetype, глифы упаковываются
stb_rect_pack (с передискретизацией 2×2 — текст чёткий в любом кегле), кернинг берётся из шрифта.
Без файлов работает встроенный ASCII-шрифт (stb_easy_font) — для отладки, тестов и как запасной вариант.
Раскладка (UTF-8, перенос по словам, выравнивание) — тоже на CPU и проверяется тестами.

```cpp
Font font = Font::load_system({.pixel_height = 32}).value_or(Font::builtin(2)); // DejaVu/Noto/Arial… или встроенный
FontHandle ui = renderer.add_font(std::move(font));
renderer.draw_text(ui, "Огненный шар", {12, 176}, {.size = 21, .align = TextAlign::Center, .max_width = 232,
                                                    .shadow = Colors::black});
glm::vec2 size = renderer.measure_text(ui, "Нанесите 6 урона.", {.size = 18, .max_width = 200});
```

## 3D

Соглашения: **Y вверх**, правая система, камера смотрит вдоль −Z. UV сеток — как везде в модуле: (0, 0) —
левый верх картинки.

```cpp
auto r3 = Renderer3D::create().value();
Mesh card = Mesh::create(MeshData::rounded_slab({1.4f, 2.0f}, 0.04f, 0.1f)); // карта со скруглёнными углами

Camera3D camera{.position = {0, 12, 9}, .target = {0, 0, 1}, .viewport = {w, h}};
Environment env;                                   // рассеянный свет + солнце
env.add_point({.position = fireball, .color = Colors::yellow, .intensity = 2, .radius = 4});

r3.clear(sky);                                     // цвет + глубина
r3.begin(camera, env);
r3.draw(card, model, {.texture = &face.color(), .uv = RenderTarget::uv()});
r3.draw_shape(Renderer3D::Shape::Sphere, bubble, {.color = {255, 214, 90, 70}, .lit = false, .blend = BlendMode::Additive});
Render3DStats stats = r3.end();                    // непрозрачные по текстурам, прозрачные — от дальних к ближним

Ray ray = camera.screen_to_ray(mouse);             // выбор мышью: луч в локальные координаты объекта
if (auto t = intersect(ray.transformed(glm::inverse(model)), card.bounds())) { /* под курсором */ }
ScreenPoint label = camera.world_to_screen(head);  // подпись в 2D-оверлее над объектом
```

- RendererSystem::Mesh хранит любую раскладку вершины (RendererSystem::GL::VertexLayout): стандартную
  RendererSystem::Vertex3D или свою (воксели: позиция + цвет).
- Сетка с известными границами (`upload(MeshData)`) отсекается пирамидой видимости камеры.
- После `end()` глубина и отсечение граней выключены — можно сразу рисовать 2D-оверлей.
- **Рендер в текстуру для 3D:** нарисуйте Renderer2D в RendererSystem::RenderTarget (лицо карты, табличка, миникарта)
  и натяните его `color()` на сетку с `uv = RenderTarget::uv()` — так кадр не перевёрнут (в OpenGL строка 0 — низ).
  `FramebufferDesc{.depth = true}` — буфер глубины для 3D в текстуру.

## Процедурные изображения

RendererSystem::Procedural — шум Перлина (stb_perlin): `perlin`, `fbm`, `ridge`, `turbulence`,
градиенты и `noise_image()`, круги и кольца со сглаженным краем. Детерминированы — можно вызывать
из задач JobSystem. RendererSystem::Image::blend() собирает картинку из слоёв, `save_png()` / `encode_png()`
пишут PNG (stb_image_write), RendererSystem::RenderTarget::read_default() читает кадр окна (скриншоты).

## Анимации в data-oriented стиле

- RendererSystem::AnimationClip — неизменяемые кадры; хранятся один раз в RendererSystem::AnimationLibrary.
- RendererSystem::AnimationState — 16 байт на сущность (компонент ECS).
- RendererSystem::advance_animations() обновляет весь массив состояний за один проход.

```cpp
ClipId walk = library.add({.name = "dwarf.walk",
                           .frames = make_grid_frames({.columns = 8, .rows = 8}, 0, 6, 0.1f)});
std::vector<AnimationState> states(count, AnimationState::start(walk));

advance_animations(states, library, dt);                // система анимаций
sprite.uv = current_uv(states[i], library);             // система отрисовки
```

## Ошибки

- Ошибки **данных** (нет файла, битое изображение, шейдер не компилируется) возвращаются
  как `std::expected<T, std::string>` — их можно показать игроку или моддеру.
- Ошибки **использования API** (пустой клип, чужой дескриптор, `begin()` без `end()`)
  бросают RendererSystem::RendererError.

## Ограничения

- Однопоточный (как и сам OpenGL-контекст). CPU-ядро можно заполнять где угодно,
  но Renderer2D / Renderer3D вызываются в потоке контекста.
- Нет instancing и persistent-mapped буферов: 2D-вершины загружаются каждый кадр (orphaning),
  3D-объекты рисуются по одному вызову на сетку.
- Нет теней, нормальных карт и PBR: освещение — Блинн–Фонг.
- Текст — растровый атлас: кегль сильно больше запечённого размывается (запекайте с запасом).

## Примеры

- `01_window_sprites.cpp` — окно движка, тайлы, анимированные существа, камера (нужен WindowSystem).
- `02_offscreen_png.cpp` — рендер в Framebuffer и сохранение в PNG (нужен WindowSystem).
- `03_data_oriented_frame.cpp` — CPU-часть кадра из массивов в стиле ECS, без GPU.
- `04_scene_3d.cpp` — 3D-сцена в PNG: процедурные фактуры, карта с лицом из Framebuffer и текстом,
  солнце и точечный свет, прозрачный купол, подпись над объектом (нужен WindowSystem).

## Сборка, тесты, бенчмарки

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRENDERERSYSTEM_SANITIZE=ON
cmake --build build
ctest --test-dir build -R RendererSystem          # GPU-тесты пропускаются, если нет OpenGL
ctest --test-dir build -R RendererSystem -LE gpu  # только то, что работает без GPU
cmake --build build --target RendererSystemDocs   # документация (нужен Doxygen)
```
