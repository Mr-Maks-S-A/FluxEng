cmake_minimum_required(VERSION 3.10)
project(imgui LANGUAGES CXX)

# 1. Основные файлы ядра ImGui
set(IMGUI_CORE_SOURCES
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
)

# 2. Выбираем только те бэкенды, которые нужны для Fluxborn
set(IMGUI_BACKEND_SOURCES
    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_opengl3.cpp
)

# Собираем всё в одну библиотеку
add_library(imgui STATIC
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_BACKEND_SOURCES}
)

# 3. Настройка инклюдов
# Мы добавляем и корень, и папку backends в PUBLIC,
# чтобы в своем коде ты мог писать #include "imgui_impl_glfw.h" без лишних путей
target_include_directories(imgui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# 4. Зависимости
# ImGui должен знать про GLFW и GLAD (которые уже есть в главном проекте)
target_link_libraries(imgui PRIVATE glfw glad::glad)

# 5. Обязательный макрос для работы через GLAD
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)

# Алиас для удобной линковки
add_library(imgui::imgui ALIAS imgui)
