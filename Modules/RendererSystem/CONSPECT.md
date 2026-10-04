# RendererSystem — конспект для изучения и переписывания

Самый большой модуль (~7000 строк, из них `VulkanDevice.cpp` 1430 и `OpenGLDevice.cpp` 510). Официальное описание — `docs/mainpage.md`. Зависимостей от модулей движка нет; снаружи `glm`, `glad`, `stb_*`, Vulkan SDK + shaderc (опционально). Окно ему не нужно — достаточно контекста GL или поверхности Vulkan.

## 1. Зачем и главная идея

2D и 3D рендер с **двумя графическими API** за одним интерфейсом. Идея в двух слоях:

| Слой | Что | Нужен GPU |
|---|---|---|
| **CPU-ядро** | `Color`, `Rect`, `Image`, `Camera2D/3D`, `SpriteBatch`, анимации, `Frustum/Ray/Aabb`, `MeshData`, `Font`, `Procedural` | нет |
| **GPU** | `RHI::Device` (OpenGL/Vulkan), `Texture`, `RenderTarget`, `Mesh`, `Pipeline`, `Renderer2D`, `Renderer3D` | да |

CPU-ядро превращает кадр в массив вершин и список команд (`DrawCommand`); бэкенд только загружает это в GPU. Поэтому логику кадра можно тестировать без видеокарты, а новый бэкенд берёт тот же результат.

Над RHI **нет ни одного вызова API**: `Renderer2D`, `Renderer3D` и игры работают на обоих бэкендах без изменений. Равенство бэкендов проверяется тестами (кадр на GL и Vulkan, сравнение пикселей).

## 2. Карта файлов (по слоям, читай снизу вверх)

| Слой | Файлы |
|---|---|
| Базовые типы | `Core/Color`, `Geometry` (Rect, UvRect), `Geometry3D` (Aabb, Ray, Frustum), `Handles`, `Error`, `Image` (+PNG), `Procedural` |
| RHI | `RHI/Types` (ручки, форматы, `PipelineDesc`, `DrawCall`), `RHI/Device` (интерфейс), `RHI/Resources` (RAII `Texture`, `RenderTarget`, `Mesh`, `Pipeline`) |
| Бэкенды | `code/RHI/OpenGLDevice.cpp`, `VulkanDevice.cpp`, `GL/Shader.*`, `Backends.hpp` |
| 2D | `Batch/SpriteBatch`, `Renderer2D`, `Text/Font`, `Animation/Animation`, `Scene/Camera2D` |
| 3D | `Mesh/MeshData`, `Scene/Camera3D`, `Renderer3D` |

## 3. Как устроено

### 3.1 Соглашения (самый частый источник ошибок)
- **2D: ось Y вниз**, (0,0) — левый верх; **UV (0,0)** — левый верхний пиксель изображения. Углы в радианах, при оси Y вниз положительный угол — по часовой.
- **3D: Y вверх**, правая система, камера смотрит вдоль −Z; обход против часовой — лицевая сторона.
- Цвет — RGBA8 (4 байта в вершине). Текстуры по умолчанию с фильтром `Nearest` (пиксель-арт, воксельные атласы).
- **Текстура цели** (`RenderTarget`) рисуется с `RenderTarget::uv()`: в OpenGL строка 0 — низ, в Vulkan — верх; `uv()` скрывает разницу.
- Проекция — в стиле OpenGL (глубина −1…1, `glm` по умолчанию); Vulkan-бэкенд переводит глубину сам (`FLUX_POSITION(clip)`), Y у Vulkan — отрицательная высота viewport.

### 3.2 RHI
- **Ручки** `BufferId/TextureId/TargetId/PipelineId` — числа, 0 = «нет» (ZII); владеют ресурсами **RAII-обёртки** `Texture/RenderTarget/Mesh/Pipeline` (только перемещаются, умирают **раньше устройства**).
- `Device` — «машина состояний» без глобального состояния драйвера: `begin_frame(w,h)` → `bind_target` → `clear` → `push_uniform(...)` → `draw(DrawCall)` → `end_frame()`.
- `DrawCall{pipeline, vertices, indices, first, count, texture, frame(UniformSlice), draw_uniforms}`; `push_uniform` кладёт блок в память кадра, срез живёт до конца кадра.
- Один **GLSL на оба бэкенда**: `#version` и макросы бэкенд добавляет сам — `FLUX_LOCATION(n)`, `FLUX_VARYING(n)`, `FLUX_UNIFORM(set, binding)` (set 0 — блок кадра, set 1 — блок вызова), `FLUX_SAMPLER(2,0)`. Vulkan компилирует через shaderc во время работы.
- Vulkan-бэкенд: **один кадр в полёте**, кольцо памяти кадра, «буфер, уже нарисованный в этом кадре, при обновлении заменяется» (аналог orphaning), ленивые проходы (очистка → loadOp = CLEAR), отложенное удаление ресурсов. Слои валидации считает `validation_messages()` — тесты требуют ноль. Без `create_surface` устройство рисует только в текстуры (тесты, офлайн).
- `Device::create(...)` → `std::expected<unique_ptr<Device>, string>`; ошибки **данных** — `expected`, ошибки **использования** (чужая ручка, рисование без кадра) — исключение `RendererError`.
- Не потокобезопасен: все вызовы из потока, создавшего устройство.

### 3.3 2D: `SpriteBatch` → `Renderer2D`
- `SpriteInstance{position, size, pivot, rotation, uv, color, texture, layer, flip}` → 4 вершины `SpriteVertex` (20 байт: позиция, uv, цвет RGBA8).
- `SpriteBatch::build()` **сортирует** по ключу и склеивает в `DrawCommand` (одна текстура — одна команда):
  - `LayerThenTexture` — внутри слоя группировка по текстуре (минимум draw calls): тайлы, воксели, существа;
  - `LayerThenSubmission` — порядок вызовов: UI, текст, точные перекрытия.
  - Сортировка **поразрядная (LSD radix)**, пропускает разряды, где у всех одинаковый байт → обычно 1–2 прохода.
- `Renderer2D`: `begin(camera)` … `draw/fill_rect/draw_rect/draw_line/draw_text` … `end()` → `RenderStats{quads, draw_calls, texture_binds}`. Вершины каждый кадр заливаются в `Stream`-буфер, индексы (6 на квад) — статический буфер, растёт по требованию. Нет instancing.
- Ресурсы (`TextureHandle`, `FontHandle`) — индексы внутри `Renderer2D`; `TextureHandle::white()` = 0 (белая 1×1).

### 3.4 Текст
`Font` — атлас глифов **на CPU**: TTF запекается `stb_truetype`, упаковка `stb_rect_pack` с передискретизацией 2×2, кернинг из шрифта; атлас растёт 512 → `max_atlas_size`, пока глифы не поместятся. Диапазоны кодовых точек по умолчанию включают ASCII, Latin-1, **кириллицу**, пунктуацию, индексы, стрелки, математику. Без файлов — встроенный ASCII-шрифт (`Font::builtin`, stb_easy_font). `load_system` ищет DejaVu/Noto/Arial. Раскладка (UTF-8, перенос по словам, выравнивание) — тоже на CPU: `layout(text, options, out_quads)`. Кегль, сильно больший запечённого, размывается — запекай с запасом (`ui_font_size = 32` в `Core`).

### 3.5 Анимации (data-oriented)
`AnimationClip` (неизменяемые кадры) лежит один раз в `AnimationLibrary`; `AnimationState` — **16 байт на сущность** (компонент ECS); `advance_animations(span<AnimationState>, library, dt)` обновляет всё за один проход; `current_uv(state, library)` отдаёт кадр отрисовке. `make_grid_frames(grid, first, count, duration)` строит кадры спрайт-листа.

### 3.6 3D
- `MeshData` (CPU: `Vertex3D` 36 байт + индексы; `box/quad/plane/sphere/cylinder/rounded_rect/rounded_slab`, `compute_normals`, `append`) → `Mesh::create(device, data)` (GPU). Сетка хранит любую раскладку вершины (`VertexLayout`) — воксели используют свою (позиция + цвет).
- `Renderer3D`: `begin(camera, Environment)` → `draw(mesh, model, Material)` → `end()`. Непрозрачные сортируются по текстурам, **прозрачные — от дальних к ближним** (расстояние до камеры). Отсечение по пирамиде видимости (`Frustum` из `Camera3D`) для сеток с границами → `Render3DStats.culled`.
- Освещение — **Блинн–Фонг**: солнце + рассеянный + до 8 точечных (`Environment::max_point_lights`), туман (`fog_start/fog_end`). Нет теней, нормальных карт, PBR.
- Материал: цвет, текстура + `uv`-область, `emissive`, `specular/shininess`, `lit`, `blend` (Opaque/Alpha/Additive), `double_sided`. 6 заранее собранных конвейеров по (смешивание × двусторонность).
- Выбор мышью: `Camera3D::screen_to_ray` → `intersect(ray.transformed(inverse(model)), mesh.bounds())`; подпись над объектом: `world_to_screen`.
- После `end()` глубина и отсечение выключены — можно сразу рисовать 2D-оверлей.

### 3.7 Процедурные изображения
`Procedural::perlin/fbm/ridge/turbulence` (stb_perlin; **float**, поэтому только для рендера, не для симуляции), `noise_image` с градиентом, `circle_image`. `Image::blend` собирает слои, `save_png/encode_png` пишут PNG.

## 4. Как использовать

```cpp
auto device   = RHI::Device::create({.backend = Backend::OpenGL}).value();
auto renderer = Renderer2D::create(*device).value();
TextureHandle goblin = renderer.load_texture("goblin.png").value();

Camera2D camera{.position = player, .zoom = 2.0f, .viewport = {w, h}};
device->begin_frame(w, h);
renderer.clear(Colors::black);
renderer.begin(camera);
renderer.draw(SpriteInstance{.position = {10, 20}, .size = {16, 16}, .texture = goblin, .layer = 1});
renderer.fill_rect({{0, 0}, {100, 4}}, Colors::red, 10);
RenderStats stats = renderer.end();
device->end_frame();

auto r3 = Renderer3D::create(*device).value();
Mesh card = Mesh::create(*device, MeshData::rounded_slab({1.4f, 2.0f}, 0.04f, 0.1f));
r3.begin(Camera3D{.position = {0,12,9}, .target = {0,0,1}, .viewport = {w,h}}, Environment{});
r3.draw(card, model, {.texture = &face.color(), .uv = RenderTarget::uv()});
r3.end();
```
В проекте им пользуются `Core::App` (создаёт устройство и оба рендера), `WorldRender` (поверхность и туман), `RuneEditor::View` (рисование графа), все игры через `app.renderer()`/`renderer3d()`.

## 5. Что менять осторожно

- **Соглашения Y/UV** (§3.1): перевёрнутая картинка почти всегда значит, что забыли `RenderTarget::uv()` или перепутали ось Y между 2D и 3D.
- Раскладки вершин (`SpriteVertex` 20 байт, `Vertex3D` 36 байт) — **контракт с шейдерами**, закреплены `static_assert`.
- Ресурсы должны умирать **раньше `Device`**: в `Core::App` порядок полей это обеспечивает; в своём коде храни `Texture/Mesh/RenderTarget` в объектах, которые разрушаются до устройства.
- Всё, что выделено `push_uniform` и `Stream`-буферами, **живёт до конца кадра** — не храни срезы между кадрами.
- Не добавляй вызовы GL/Vulkan вне `RHI/*Device.cpp` — тогда пропадёт равенство бэкендов.
- Изменение шейдера = проверить на **обоих** бэкендах (тест пиксельного сравнения).
- 2D-вершины заливаются каждый кадр целиком: тысячи спрайтов нормально, сотни тысяч — упрёшься в загрузку (нужен instancing — идея в конспекте 8).
- Однопоточный: CPU-ядро можно заполнять где угодно, но `Renderer2D/3D` вызывай из потока устройства.

## 6. Упражнения для переписывания

1. Напиши третий бэкенд-заглушку `NullDevice` (пишет вызовы в журнал) и прогони `Renderer2D` на нём — какие методы `Device` реально нужны 2D?
2. Добавь instancing для спрайтов: что изменится в `SpriteBatch`, `SpriteVertex` и шейдере, и как это сохранит равенство бэкендов?
3. Реализуй тени (shadow map) для солнца в `Renderer3D` — какие части RHI придётся расширить (глубинная цель как текстура)?
4. Сделай атласный менеджер текстур (упаковка `stb_rect_pack` в рантайме) вместо `TextureHandle` на каждую картинку.
5. Перепиши `advance_animations` с SIMD и сравни по бенчмарку.
6. Прочитай `VulkanDevice.cpp` в таком порядке: создание инстанса/устройства → swapchain → кольцо памяти кадра → `begin_frame/end_frame` → конвейеры → `draw`. Нарисуй схему «один кадр в полёте».

## 7. Тесты и примеры

`Modules/RendererSystem/tests` (8 файлов; GPU-тесты помечены `gpu`, идут под xvfb; пиксельное сравнение GL/Vulkan), 4 примера (`01_window_sprites`, `02_offscreen_png`, `03_data_oriented_frame` — без GPU, `04_scene_3d`), бенчмарки. Запуск без GPU: `ctest -R RendererSystem -LE gpu`.
