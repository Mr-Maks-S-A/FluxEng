#pragma once
/**
 * @file Types.hpp
 * @brief Общие типы окна: размер, графический API, режим курсора, бэкенд платформы, параметры создания.
 * Ни GLFW, ни glad, ни других платформенных заголовков здесь нет и быть не должно.
 */

#include <cstdint>
#include <string>

namespace WindowSystem {

/// @brief Размер в пикселях.
struct Size {
    int width = 0;  ///< Ширина.
    int height = 0; ///< Высота.
    friend bool operator==(Size, Size) noexcept = default;
};

/// @brief Какой графический API будет рисовать в окно.
enum class ClientApi : std::uint8_t {
    OpenGL, ///< Окно с контекстом OpenGL (gl_major.gl_minor core), функции загружены.
    None,   ///< Без контекста: для Vulkan (поверхность — create_vulkan_surface()).
};

/// @brief Режим курсора.
enum class CursorMode : std::uint8_t {
    Normal,   ///< Обычный курсор.
    Hidden,   ///< Невидим над окном, но двигается свободно.
    Captured, ///< Скрыт и захвачен: относительное движение без упора в край (обзор мышью в 3D).
};

/// @brief Платформа, на которой живёт окно.
enum class WindowBackend : std::uint8_t {
    Glfw,     ///< Настоящее окно ОС (GLFW): Windows, Linux (X11, Wayland), macOS.
    Headless, ///< Окно без экрана: размеры из конфигурации, события только внедрённые. Нужен CI, серверу, автотестам ввода; графики нет.
};

/// @brief Параметры окна.
struct WindowConfig {
    std::string title = "FluxEng"; ///< Заголовок.
    int width = 1280;              ///< Ширина окна (в экранных координатах).
    int height = 720;              ///< Высота окна.
    bool visible = true;           ///< false — скрытое окно (тесты, рендер в текстуру).
    bool resizable = true;         ///< Можно ли менять размер мышью.
    bool vsync = true;             ///< Синхронизация с частотой монитора.
    int gl_major = 3;              ///< Версия OpenGL (Core Profile): старшая.
    int gl_minor = 3;              ///< Версия OpenGL: младшая.
    int samples = 0;               ///< MSAA; 0 — выключено.
    bool close_on_escape = false;  ///< Закрывать окно по Esc (удобно для примеров; в игре решает она сама).
    ClientApi api = ClientApi::OpenGL; ///< OpenGL-контекст или окно для Vulkan. Headless всегда без графики.
    WindowBackend backend = WindowBackend::Glfw; ///< Платформа.
};

} // namespace WindowSystem
