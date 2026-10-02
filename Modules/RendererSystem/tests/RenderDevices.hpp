#pragma once
/**
 * @file RenderDevices.hpp
 * @brief Устройства RHI для GPU-тестов: OpenGL (скрытое окно GLFW) и Vulkan (без окна, со слоями валидации).
 *
 * Каждый GPU-тест прогоняется на всех доступных бэкендах — так проверяется главное обещание RHI:
 * одинаковый результат на OpenGL и Vulkan. Недоступный бэкенд (нет дисплея, нет Vulkan) пропускается
 * с сообщением. Устройства живут до конца процесса (как GlTestContext).
 */

#include "GlTestContext.hpp"

#include <RendererSystem/RendererSystem.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

namespace RendererTests {

struct TestDevice {
    std::string name;
    RendererSystem::RHI::Device* device = nullptr;
};

inline std::vector<TestDevice>& test_devices() {
    static std::vector<TestDevice> devices = [] {
        std::vector<TestDevice> out;
        if (GlTestContext::instance().available()) {
            auto gl = RendererSystem::RHI::Device::create({.backend = RendererSystem::Backend::OpenGL});
            if (gl) out.push_back({"opengl", gl->release()});
        }
        if (RendererSystem::backend_compiled(RendererSystem::Backend::Vulkan)) {
            auto vk = RendererSystem::RHI::Device::create({.backend = RendererSystem::Backend::Vulkan, .validation = true});
            if (vk) {
                out.push_back({"vulkan", vk->release()});
            } else {
                MESSAGE("Vulkan device is not available: " << vk.error());
            }
        }
        return out;
    }();
    if (GlTestContext::instance().available()) GlTestContext::instance().make_current();
    return devices;
}

/// Кадр устройства на время теста: begin_frame / end_frame.
class Frame {
public:
    explicit Frame(RendererSystem::RHI::Device& device, int size = 64) : m_device(device) { m_device.begin_frame(size, size); }
    ~Frame() { m_device.end_frame(); }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

private:
    RendererSystem::RHI::Device& m_device;
};

/**
 * @brief Выполняет `fn(device)` на каждом доступном бэкенде (в сообщениях об ошибках — имя бэкенда)
 * и проверяет, что слои валидации Vulkan ничего не сообщили.
 */
template<typename Fn>
void for_each_backend(Fn&& fn) {
    auto& devices = test_devices();
    if (devices.empty()) {
        MESSAGE("no GPU backend available — GPU test skipped");
        return;
    }
    for (const TestDevice& d : devices) {
        INFO("backend: " << d.name);
        const std::size_t before = d.device->validation_messages();
        fn(*d.device);
        CHECK_MESSAGE(d.device->validation_messages() == before, "Vulkan validation reported problems");
    }
}

} // namespace RendererTests
