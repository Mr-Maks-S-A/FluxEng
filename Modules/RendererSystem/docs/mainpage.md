# RendererSystem — 2D-рендер FluxEng {#mainpage}

Батчевый 2D-рендер на OpenGL 3.3 core: спрайты, спрайт-листы, анимации, прямоугольники,
линии, камера, рендер в текстуру.

Подключение: `#include <RendererSystem/RendererSystem.hpp>`, CMake-цель `engine::RendererSystem`.
Зависимости библиотеки: `glm`, `glad`, `stb`. Окно и GLFW ей не нужны — достаточно текущего GL-контекста.

## Два слоя

| Слой | Что внутри | Нужен GPU |
|---|---|---|
| **CPU-ядро** | RendererSystem::Color, RendererSystem::Rect, RendererSystem::Image, RendererSystem::Camera2D, RendererSystem::SpriteBatch, анимации | нет |
| **GL-бэкенд** | RendererSystem::GL::Shader, RendererSystem::GL::Texture, RendererSystem::GL::Framebuffer, RendererSystem::Renderer2D | да |

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
  но Renderer2D вызывается в потоке контекста.
- Текста пока нет: нужен атлас глифов (stb_truetype уже есть в ExternalLibrary).
- Нет instancing и persistent-mapped буферов: вершины загружаются каждый кадр (orphaning).

## Примеры

- `01_window_sprites.cpp` — окно движка, тайлы, анимированные существа, камера (нужен WindowSystem).
- `02_offscreen_png.cpp` — рендер в Framebuffer и сохранение в PNG (нужен WindowSystem).
- `03_data_oriented_frame.cpp` — CPU-часть кадра из массивов в стиле ECS, без GPU.

## Сборка, тесты, бенчмарки

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRENDERERSYSTEM_SANITIZE=ON
cmake --build build
ctest --test-dir build -R RendererSystem          # GPU-тесты пропускаются, если нет OpenGL
ctest --test-dir build -R RendererSystem -LE gpu  # только то, что работает без GPU
cmake --build build --target RendererSystemDocs   # документация (нужен Doxygen)
```
