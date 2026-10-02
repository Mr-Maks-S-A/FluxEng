#include "Backends.hpp"


namespace RendererSystem {

std::string_view to_string(Backend backend) noexcept {
    return backend == Backend::Vulkan ? "vulkan" : "opengl";
}

std::optional<Backend> parse_backend(std::string_view name) noexcept {
    if (name == "gl" || name == "opengl" || name == "OpenGL") return Backend::OpenGL;
    if (name == "vk" || name == "vulkan" || name == "Vulkan") return Backend::Vulkan;
    return std::nullopt;
}

bool backend_compiled([[maybe_unused]] Backend backend) noexcept {
#if defined(FLUX_RENDERER_VULKAN)
    return true;
#else
    return backend == Backend::OpenGL;
#endif
}

std::vector<Backend> compiled_backends() {
    std::vector<Backend> out{Backend::OpenGL};
    if (backend_compiled(Backend::Vulkan)) out.push_back(Backend::Vulkan);
    return out;
}

namespace RHI {

std::expected<std::unique_ptr<Device>, std::string> Device::create(const DeviceConfig& config) {
    switch (config.backend) {
        case Backend::OpenGL: return make_opengl_device(config);
        case Backend::Vulkan:
#if defined(FLUX_RENDERER_VULKAN)
            return make_vulkan_device(config);
#else
            return std::unexpected(std::string("RendererSystem was built without Vulkan (Vulkan SDK / shaderc not found)"));
#endif
    }
    return std::unexpected(std::string("unknown backend"));
}

} // namespace RHI
} // namespace RendererSystem
