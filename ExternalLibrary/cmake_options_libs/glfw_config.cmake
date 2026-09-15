# ==============================================================================
# PROJECT: Fluxborn Engine
# MODULE:  GLFW 3.4+ Configuration (Platform Abstraction Layer)
# ROLE:    Fine-tuning windowing and context creation
# ==============================================================================

# --- 1. АВТООПРЕДЕЛЕНИЕ ВЕРСИИ ---
# Извлекаем версию напрямую из исходников для честных логов
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/glfw/CMakeLists.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/glfw/CMakeLists.txt"
         GLFW_PROJECT_LINE REGEX "project\\(GLFW VERSION ([0-9.]+)")
    string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" FLUX_GLFW_VERSION "${GLFW_PROJECT_LINE}")
else()
    set(FLUX_GLFW_VERSION "Unknown")
endif()


# --- 2. ОПТИМИЗАЦИЯ СБОРКИ (BUILD POLICY) ---
# Отключаем всё лишнее, чтобы сократить время компиляции
set(GLFW_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS        OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL           OFF CACHE BOOL "" FORCE)

# Собираем статически для монолитного бинарника Fluxborn
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)


# --- 3. ПЛАТФОРМОЗАВИСИМЫЕ НАСТРОЙКИ ---

# --- LINUX (CachyOS / Arch) ---
if(UNIX AND NOT APPLE)
    message(STATUS "[GLFW] Backend: Hybrid Wayland + X11 (Detected v${FLUX_GLFW_VERSION})")

    # Включаем современный стек для CachyOS
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "Enable Wayland" FORCE)
    set(GLFW_BUILD_X11     ON CACHE BOOL "Enable X11"     FORCE)

    # Используем динамическую загрузку библиотек (runtime), чтобы бинарник
    # запускался и на чистом Wayland, и на X11 без перекомпиляции.
    set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)
endif()

# --- macOS (Cocoa) ---
if(APPLE)
    message(STATUS "[GLFW] Backend: Cocoa (Native macOS)")
    set(GLFW_COCOA_RETINA ON CACHE BOOL "High-DPI Support" FORCE)
    set(GLFW_COCOA_CHDIR  ON CACHE BOOL "Auto-chdir to Resources" FORCE)
endif()

# --- WINDOWS (Win32) ---
if(MSVC)
    # Гарантируем совместимость рантайма (MD/MT) с основным движком
    set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)
endif()


# --- 4. КОРРЕКЦИЯ ИНКЛЮДОВ (GLFW INTERNAL FIX) ---
# GLFW 3.4+ иногда требует доступа к своим сгенерированным конфигам внутри build-директории.
# Мы создаем список путей, которые пробросим в таргет в основном External/CMakeLists.txt
set(FLUX_GLFW_INTERNAL_INCLUDES
    "${CMAKE_CURRENT_BINARY_DIR}/glfw/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/glfw/src"
)


# --- 5. ФИНАЛЬНЫЙ СТАТУС ---
message(STATUS "[External] GLFW ${FLUX_GLFW_VERSION}: Configuration locked.")

# Убираем временные переменные из глобальной видимости (опционально)
unset(GLFW_PROJECT_LINE)
