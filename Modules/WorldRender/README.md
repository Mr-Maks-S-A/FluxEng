# WorldRender — отрисовка мира

Камеры, поверхность из чанков, туман, отладочные линии, помощники оверлея. **Рендер не знает, что именно он рисует**: данные
приходят через интерфейсы `SurfaceSource` (сетка чанков и их сетки) и `FogSource` (ячейки и яркость), поэтому модуль не зависит
от `Terrain` и `ManaField`: планеты, другие миры и LOD подключаются новым источником.

Подключение: `#include <WorldRender/WorldRender.hpp>`, цель `engine::WorldRender` (зависит от `Math`, `JobSystem`, `RendererSystem`, glm).
Готовые источники для ландшафта и поля маны — цель `engine::WorldRenderTerrain` (`WorldRender/Adapters/Terrain.hpp`).

## Отрисовка относительно камеры

Положение камеры — `double`; на GPU уходят только разности «точка − глаз» (`View::relative`): вершины лежат от угла чанка, сам чанк
смещён разностью двойной точности, поэтому float не теряет точность вдали от начала координат.

```cpp
Terrain::SdfWorld world(1);  ManaField::ManaGrid mana;
WorldRender::TerrainSurface surface(world);                // адаптеры: SurfaceSource / FogSource
WorldRender::ManaFogSource  fog(mana, world);
WorldRender::TerrainView terrain_view(device, surface);
WorldRender::FogView     fog_view(device);

terrain_view.enqueue(WorldRender::TerrainSurface::indices(world.take_dirty()));     // изменённые чанки — в очередь
terrain_view.update(jobs, eye, /*budget*/ 4);              // строит ≤ 4 сеток за кадр параллельно, ближние первыми; 0 — все сразу
auto view = rig.view(head_position, world, viewport);      // камера: следящая или свободная; любой Math::SdfField не пускает её в землю
terrain_view.draw(view, WorldRender::Light{});
fog_view.draw(fog, view);                                  // билборды: яркость = плотность / база, дыра от заклинания видна
```

| Часть | Что |
|---|---|
| `Sources.hpp` | `GridShape`, `SurfaceVertex/SurfaceMesh`, `SurfaceSource`, `FogSource`, `CellBox` |
| `TerrainView`, `MeshQueue` | очередь перестройки без повторов, загрузка на GPU, цвет по высоте и уклону, каркас, рамки чанков |
| `FogView`, `build_fog_vertices` | туман из ячеек; выбор ячеек — чистая функция, тестируется без GPU |
| `CameraRig` | следящая камера (не заходит в поверхность) и свободная; `View` — глаз `dvec3` и относительная `Camera3D` |
| `DebugDraw`, `LineRenderer` | линии, рамки, кресты; накапливаются за кадр |
| `overlay::` | панель текста, полоса, прицел, тепловая карта, сглаживание `Smoothed` |

Примеры: `01_data_sources.cpp` (без GPU: свои источники, очередь, туман, камеры), `02_render_terrain_png.cpp` (весь кадр в PNG
без видимого окна). Тесты: `WorldRenderTests` (без Terrain, на заглушках) и `WorldRenderAdapterTests` (адаптеры).
