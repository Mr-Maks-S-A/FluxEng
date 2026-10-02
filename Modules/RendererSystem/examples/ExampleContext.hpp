#pragma once
/**
 * @file ExampleContext.hpp
 * @brief Окно WindowSystem + устройство RHI выбранного бэкенда — общий запуск примеров (`--backend gl|vulkan`).
 */

#include <RendererSystem/RendererSystem.hpp>
#include <WindowSystem/Window.hpp>

#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace Example {

/// Окно и устройство: окно объявлено первым — устройство умирает раньше него.
struct Context {
    WindowSystem::Window window;
    std::unique_ptr<RendererSystem::RHI::Device> device;
};

/// `--backend gl|vulkan` (по умолчанию OpenGL).
inline RendererSystem::Backend backend_from_args(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--backend") {
            if (const auto backend = RendererSystem::parse_backend(argv[i + 1])) return *backend;
        }
    }
    return RendererSystem::Backend::OpenGL;
}

/// Открывает окно под бэкенд и создаёт устройство.
inline std::expected<Context, std::string> open(const std::string& title, int width, int height, bool visible,
                                                RendererSystem::Backend backend) {
    using namespace RendererSystem;
    const bool vulkan = backend == Backend::Vulkan;
    auto window = WindowSystem::Window::create({.title = std::format("{} [{}]", title, to_string(backend)), .width = width, .height = height,
                                                .visible = visible, .vsync = visible, .close_on_escape = true,
                                                .api = vulkan ? WindowSystem::ClientApi::None : WindowSystem::ClientApi::OpenGL});
    if (!window) return std::unexpected(window.error());
    Context context{std::move(*window), nullptr};
    RHI::DeviceConfig config{.backend = backend, .vsync = visible};
    if (vulkan && visible) { // невидимому окну swapchain не нужен: рисуем в цели
        config.instance_extensions = WindowSystem::Window::vulkan_instance_extensions();
        config.create_surface = [&window = context.window](std::uintptr_t instance) { return window.create_vulkan_surface(instance); };
    }
    auto device = RHI::Device::create(config);
    if (!device) return std::unexpected(device.error());
    context.device = std::move(*device);
    return context;
}

} // namespace Example
