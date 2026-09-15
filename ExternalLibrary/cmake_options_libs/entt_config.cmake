# ==============================================================================
# PROJECT: Fluxborn Engine
# MODULE:  EnTT (Entity Component System)
# ROLE:    High-performance Data-Oriented Design (DoD) orchestration
# ==============================================================================

# --- 1. БЕЗОПАСНОСТЬ И ОТЛАДКА ---
set(FLUX_ENTT_DEFINITIONS "")

# Включаем проверку ассертов только для Debug-сборки.
# Это ловит ошибки типа "удаление несуществующей сущности" на ранних этапах.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND FLUX_ENTT_DEFINITIONS ENTT_ASSERT_LEVEL=1)
endif()

# --- 2. ОПТИМИЗАЦИЯ (RELEASE) ---
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Можно добавить ENTT_NO_DTOA для ускорения сериализации, если не важна точность float в JSON
    # list(APPEND FLUX_ENTT_DEFINITIONS ENTT_NO_DTOA)
endif()

# --- 3. ФИНАЛЬНЫЙ СТАТУС ---
message(STATUS "[EnTT Config] Profile: ECS ready (Assert Level: ${CMAKE_BUILD_TYPE}).")
