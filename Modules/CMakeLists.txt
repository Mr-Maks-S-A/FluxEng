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
add_subdirectory( Core )           # слой приложения: зависит от модулей выше
