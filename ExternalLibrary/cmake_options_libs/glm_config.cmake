# ==============================================================================
# PROJECT: Fluxborn Engine
# MODULE:  GLM Optimization Profile (Zen 2 / Ryzen 3700X)
# ROLE:    High-performance math configuration for Voxel Workloads
# ==============================================================================

# --- 1. СТАНДАРТ И БЕЗОПАСНОСТЬ ---
# Активируем современные возможности C++20 для шаблонов GLM
set(GLM_ENABLE_CXX_20            ON  CACHE BOOL "" FORCE)
set(GLM_ENABLE_LANG_EXTENSIONS   OFF CACHE BOOL "" FORCE)


# --- 2. АППАРАТНОЕ УСКОРЕНИЕ (SIMD) ---
# Zen 2 отлично справляется с 256-битными инструкциями.
# Это критично для обсчета чанков и систем освещения.
set(GLM_ENABLE_SIMD_AVX2         ON  CACHE BOOL "" FORCE)


# --- 3. ОПТИМИЗАЦИЯ СБОРКИ ---
set(GLM_BUILD_TESTS              OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL            OFF CACHE BOOL "" FORCE)


# --- 4. МАТЕМАТИЧЕСКАЯ ЛОГИКА (DEFINITIONS) ---
# Мы собираем список определений, которые будут проброшены через INTERFACE таргет.
# Это Modern CMake Way: настройки "текут" вместе с библиотекой.
set(FLUX_GLM_DEFINITIONS
    GLM_FORCE_ALIGNED_GENTYPES   # Выравнивание для SIMD/DoD
    GLM_FORCE_RADIANS            # Защита от ошибок с градусами
    GLM_FORCE_DEPTH_ZERO_TO_ONE  # Современный Z-buffer (Vulkan/Reverse-Z style)
    GLM_ENABLE_FAST_MATH         # Агрессивная оптимизация float (IEEE 754 relax)
)


# --- 5. ФИНАЛЬНЫЙ СТАТУС ---
message(STATUS "[GLM Config] Profile loaded: AVX2, FastMath, Radian-Only (Standard: C++20).")
