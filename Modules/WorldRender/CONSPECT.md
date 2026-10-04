# WorldRender — конспект для изучения и переписывания

`src/include/WorldRender/{Sources,TerrainView,FogView,Camera,DebugDraw,HeightMap,Overlay,WorldRender}.hpp` + `Adapters/Terrain.hpp`, `src/code/{TerrainView,FogView,Camera,DebugDraw,HeightMap}.cpp` (~1200 строк). Зависит от `Math`, `JobSystem`, `RendererSystem`, `glm`, `ManaField`/`Terrain` — **только через адаптеры** (цель `WorldRenderTerrain`).

## 1. Зачем и главная идея

Отрисовка 3D-мира: поверхность из чанков, туман маны, отладочные линии, камеры, карта высот, помощники оверлея. Главное решение — **рендер не знает, что он рисует**. Данные приходят через интерфейсы:
- `SurfaceSource` — сетка чанков и построение сетки чанка;
- `FogSource` — ячейки и яркость тумана;
- `HeightSource` — высота над (x, z) для карты сверху.

Поэтому модуль не зависит от `Terrain` и `ManaField`: планеты, другие миры, LOD подключаются новым источником. Готовые источники для ландшафта и поля маны — цель `WorldRenderTerrain` (`Adapters/Terrain.hpp`).

## 2. Карта файлов

| Файл | Что |
|---|---|
| `Sources.hpp` | `ChunkIndex`, `GridShape`, `SurfaceVertex/SurfaceMesh`, `SurfaceSource`, `CellBox`, `FogSource` |
| `TerrainView.hpp/.cpp` | `MeshQueue`, `Light`, `TerrainStats`, `TerrainView` (очередь перестройки, загрузка на GPU, рисование чанков) |
| `FogView.hpp/.cpp` | `FogVertex`, `build_fog_vertices` (чистая функция), `FogView` |
| `Camera.hpp/.cpp` | `View` (глаз `dvec3` + `Camera3D`), `CameraRig` (следящая/свободная), `look_direction` |
| `DebugDraw.hpp/.cpp` | `DebugDraw` (накопитель линий), `LineRenderer` |
| `HeightMap.hpp/.cpp` | `HeightSource`, `CellRegion`, `HeightShading`, `HeightMap` (раскраска рельефа в `Image`) |
| `Overlay.hpp` | `overlay::panel/bar/crosshair/heatmap`, `Smoothed` |
| `Adapters/Terrain.hpp` | `TerrainSurface`, `TerrainHeights`, `ManaFogSource` |

## 3. Как устроено

### 3.1 Отрисовка относительно камеры (точность вдали от начала)
Позиция камеры — `double` (`View::eye`). На GPU уходят **только разности «точка − глаз»** (`View::relative`, float): вершины лежат от угла чанка (`SurfaceVertex.position` — метры от угла чанка), а сам чанк смещается разностью двойной точности. Float не теряет точность далеко от начала координат (проблема «дрожащего мира» на больших расстояниях).

### 3.2 `GridShape` и источник поверхности
`GridShape{chunks_x/y/z, chunk_meters, origin[3]}` — решётка чанков (`index`, `coord`, `chunk_origin` в `dvec3`). `SurfaceSource::build(chunk, out)` обязан быть **потокобезопасным по чтению** — его зовут параллельно. Раскладка `SurfaceVertex` (28 байт) — контракт с шейдером (`static_assert`); у `Terrain::Vertex` та же раскладка, адаптер копирует `memcpy`.

### 3.3 `TerrainView`
- `enqueue(chunks)` — изменённые чанки в **`MeshQueue`** (без повторов: `m_queued`). `pop_nearest(count, eye)` отдаёт ближайшие к глазу первыми.
- `update(jobs, eye, budget=4)`: берёт ≤ `budget` ближайших (0 — все); строит сетки **параллельно** `parallel_for(count, grain=1)` — каждая задача пишет в свой `SurfaceMesh`; затем в главном потоке заливает в `Mesh` (создаёт при первом разе, `Dynamic`). Статистика: `built`, `queued`, `build_ms`, `triangles`, `gpu_bytes`. Бюджет по кадрам сглаживает «стартовую сборку» (первый кадр ~28 мс).
- `draw(view, light)`: `push_uniform(FrameUniforms)` (матрица, солнце, ambient, цвет и дальность тумана); для каждого чанка — отсечение по `Frustum` (AABB в относительных координатах), `DrawUniforms{relative origin, origin.y}`; `stats.drawn/culled`. Режим каркаса: индексы линий строятся по требованию из CPU-копии сетки (по три ребра на треугольник).
- Цвет по высоте и уклону считается в шейдере (`material` + нормаль). `add_chunk_boxes` — рамки чанков в `DebugDraw`.

### 3.4 Туман
`FogSource{cell_meters, bounds, density(x,y,z)}`; `build_fog_vertices(source, eye, radius, out)` — **чистая функция** (тестируется без GPU): выбирает ячейки в радиусе и строит билборды `FogVertex{center от глаза, corner, density}`. `FogView::draw` рисует с аддитивным смешиванием (`intensity` 0,11 при базовой плотности, `size` 2,8 м — с перекрытием, `radius` 36 м); яркость = плотность / база, поэтому «дыра» от заклинания видна как тёмное пятно. `ManaFogSource` показывает только слой вдоль земли (`SDF ∈ [−1, ground_layer]`) — одна выборка ландшафта на ячейку.

### 3.5 Камера
`CameraRig`: режимы `Follow` (следит за персонажем на `distance`, **не заходит в поверхность** — проверка по любому `Math::SdfField`) и `Free` (полёт `free_speed` м/с). `look(dx, dy)`/`fly(...)`/`toggle()`; `view(target, ground, viewport)` возвращает `View`. Углы — `float` (это отображение, не симуляция).

### 3.6 Карта высот
`HeightMap(width, depth, cell_meters)`; `update(source)` целиком или `update(source, CellRegion)` — **частично** (после правки обновляются только клетки изменённых чанков: `TerrainHeights::cells_of_chunk`); `shade(HeightShading)` — `Image` с раскраской рельефа и освещением (азимут/высота солнца, усиление), готовый для `Renderer2D::update_texture`. `NaN` высоты (нет земли) трактуется как «пусто».

### 3.7 Оверлей и отладка
`overlay::panel` (панель текста с фоном), `bar` (полоса), `crosshair`, `heatmap`, `Smoothed` (сглаживание показателей). `DebugDraw` копит линии `line/box/cross` за кадр (`dvec3`), `LineRenderer::flush(lines, view)` рисует их с относительными координатами.

## 4. Как использовать

```cpp
Terrain::SdfWorld world(1);  ManaField::ManaGrid mana;
WorldRender::TerrainSurface surface(world);                 // адаптеры: SurfaceSource / FogSource
WorldRender::ManaFogSource  fog(mana, world);
WorldRender::TerrainView terrain_view(device, surface);
WorldRender::FogView     fog_view(device);
WorldRender::CameraRig   rig;

// каждый кадр:
terrain_view.enqueue(WorldRender::TerrainSurface::indices(sim.take_dirty_chunks()));   // изменённые чанки
terrain_view.update(jobs, eye, /*budget*/ 4);
auto view = rig.view(head_position, world, viewport);
terrain_view.draw(view, WorldRender::Light{});
fog_view.draw(fog, view);

WorldRender::HeightMap heights(128, 128);  WorldRender::TerrainHeights src(world);
heights.update(src);  auto image = heights.shade({.exaggeration = 2.0});   // карта высот для вида сверху
```
Где это в проекте: `FirstSpell` (3D вид), `RuneCell2` (карта высот сверху + туман), `ModuleProof` (карта высот на экране).

## 5. Что менять осторожно

- **`SurfaceSource::build` вызывается параллельно** — внутри нельзя писать в общее состояние; адаптер использует `thread_local` буфер (`Terrain::mesh_chunk` без аллокаций).
- Не клади `float`/`double` из этого модуля в симуляцию: здесь всё — отображение; а симуляция сюда только отдаёт данные.
- Контракт вершин (28 / 24 / 16 байт) закреплён `static_assert` и совпадает с шейдерами — менять вместе.
- Очередь сеток и бюджет `update` — компромисс: слишком маленький бюджет → «лысые» чанки после большой правки; слишком большой → скачок кадра.
- Туман — аддитивные билборды (без сортировки): порядок не важен, но много перекрытий = заливка; `radius`, `size`, `intensity` связаны.
- `CameraRig` использует SDF земли для защиты от заглубления — если передашь не тот `SdfField`, камера пройдёт сквозь рельеф.
- Неделимые ресурсы GPU (`Mesh`, `Pipeline`) должны умирать до устройства (см. `RendererSystem`).

## 6. Упражнения для переписывания

1. Напиши свой `SurfaceSource` для плоскости/волн (без `Terrain`) и покажи на нём `TerrainView` — что придётся сделать с `FogSource`?
2. Добавь LOD: два уровня `GridShape` (близко подробно, далеко грубо) — как избежать швов (подсказка: Transvoxel в README `Terrain`)?
3. Замени туман-билборды объёмным рендером (raymarch по плотности) и сравни стоимость.
4. Сделай мини-карту в реальном времени: `HeightMap` + `update(source, region)` при каждой правке — измерь, что дороже.
5. Добавь тени для солнца в `TerrainView::draw` (потребует глубинную цель в RHI).

## 7. Тесты и примеры

`Modules/WorldRender/tests`: `WorldRenderTests` (без Terrain, на заглушках — очередь, туман, камеры, карта высот) и `WorldRenderAdapterTests` (адаптеры), 2 примера: `01_data_sources.cpp` (без GPU), `02_render_terrain_png.cpp` (весь кадр в PNG без видимого окна).
