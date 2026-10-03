# Подключение всех модулей движка.
#
# Каждый модуль — самостоятельный проект: его можно собрать и отдельно
# (cmake -S Modules/<Name>), см. cmake/FluxModule.cmake.
# Порядок: сначала модули без зависимостей, затем те, что от них зависят.
add_subdirectory( MemorySystem )     # аллокаторы (ZII): без зависимостей
add_subdirectory( JobSystem )        # задачи и параллельные циклы: MemorySystem
add_subdirectory( EventSystem )
add_subdirectory( WindowSystem )
add_subdirectory( ECSSystem )
add_subdirectory( RendererSystem )
# Игровой слой «Первого заклинания» (пре-альфа): числа, запись, ландшафт, мана, руны, персонаж, симуляция, отрисовка.
add_subdirectory( Math )           # Fixed Q16.16, WorldPos, sqrt, Rng, хеш, шум, FLUX_ASSERT: без зависимостей
add_subdirectory( Phases )         # расписание фаз тика с замерами: Math
add_subdirectory( EventLog )       # журнал событий с избыточностью (Рид—Соломон): Math; адаптер шины — EventSystem
add_subdirectory( Replay )         # запись и повтор команд, FlightRecorder: Math
add_subdirectory( ManaField )      # поле маны: Math
add_subdirectory( Terrain )        # SDF-ландшафт, сетка поверхности: Math, MemorySystem
add_subdirectory( Runes )          # язык рун, стековая машина: Math, ECSSystem, EventSystem
add_subdirectory( RuneEditor )     # редактор графов рун без GPU: правки как данные, undo, диагностика, мышь: Runes, EventLog
add_subdirectory( Character )      # персонаж-капсула: Math, ECSSystem, Terrain
add_subdirectory( SpellSim )       # порядок тика, команды, весь мир: все модули выше
add_subdirectory( WorldRender )    # камеры, ландшафт, туман, линии, оверлей: Terrain, ManaField, RendererSystem, JobSystem
add_subdirectory( Core )           # слой приложения: зависит от модулей выше

# ------------------------------------------------------------------------------
# Зонтичные цели: игре достаточно подключить одну, а не десять. Модули при этом остаются отдельными —
# каждый собирается и тестируется сам по себе (tools/check_modules.sh).
#
#   engine::Foundation    Math, MemorySystem, JobSystem, Phases      числа, память, потоки, расписание тика
#   engine::Simulation    Foundation + EventSystem, ECSSystem,       детерминированное ядро: события, сущности,
#                         Replay, EventLog                           запись/повтор, журнал с избыточностью
#   engine::Platform      WindowSystem, RendererSystem, Core         окно, графика, каркас приложения
#   engine::Gameplay      Simulation + Terrain, ManaField, Runes,    правила мира заклинаний
#                         RuneEditor, Character, SpellSim
#   engine::Presentation  Platform + WorldRender (+ адаптеры),        отрисовка мира и редактора рун
#                         RuneEditorView
#   engine::FluxEng       всё вместе
# ------------------------------------------------------------------------------
add_library(FluxFoundation INTERFACE)
add_library(engine::Foundation ALIAS FluxFoundation)
target_link_libraries(FluxFoundation INTERFACE engine::Math engine::MemorySystem engine::JobSystem engine::Phases)

add_library(FluxSimulation INTERFACE)
add_library(engine::Simulation ALIAS FluxSimulation)
target_link_libraries(FluxSimulation INTERFACE engine::Foundation engine::EventSystem engine::ECSSystem engine::Replay engine::EventLog)

add_library(FluxPlatform INTERFACE)
add_library(engine::Platform ALIAS FluxPlatform)
target_link_libraries(FluxPlatform INTERFACE engine::WindowSystem engine::RendererSystem engine::Core)

add_library(FluxGameplay INTERFACE)
add_library(engine::Gameplay ALIAS FluxGameplay)
target_link_libraries(FluxGameplay INTERFACE engine::Simulation engine::Terrain engine::ManaField engine::Runes engine::RuneEditor engine::Character engine::SpellSim)

add_library(FluxPresentation INTERFACE)
add_library(engine::Presentation ALIAS FluxPresentation)
target_link_libraries(FluxPresentation INTERFACE engine::Platform engine::WorldRender engine::WorldRenderTerrain engine::RuneEditorView)

add_library(FluxEngAll INTERFACE)
add_library(engine::FluxEng ALIAS FluxEngAll)
target_link_libraries(FluxEngAll INTERFACE engine::Gameplay engine::Presentation)
