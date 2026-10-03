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
add_subdirectory( Replay )         # запись и повтор команд, FlightRecorder: Math
add_subdirectory( ManaField )      # поле маны: Math
add_subdirectory( Terrain )        # SDF-ландшафт, сетка поверхности: Math, MemorySystem
add_subdirectory( Runes )          # язык рун, стековая машина: Math, ECSSystem, EventSystem
add_subdirectory( Character )      # персонаж-капсула: Math, ECSSystem, Terrain
add_subdirectory( SpellSim )       # порядок тика, команды, весь мир: все модули выше
add_subdirectory( WorldRender )    # камеры, ландшафт, туман, линии, оверлей: Terrain, ManaField, RendererSystem, JobSystem
add_subdirectory( Core )           # слой приложения: зависит от модулей выше
