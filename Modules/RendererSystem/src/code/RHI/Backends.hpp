#pragma once
/**
 * @file Backends.hpp
 * @brief Внутреннее: фабрики бэкендов и общий заголовок шейдеров.
 */

#include <RendererSystem/RHI/Device.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace RendererSystem::RHI {

[[nodiscard]] std::expected<std::unique_ptr<Device>, std::string> make_opengl_device(const DeviceConfig& config);
#if defined(FLUX_RENDERER_VULKAN)
[[nodiscard]] std::expected<std::unique_ptr<Device>, std::string> make_vulkan_device(const DeviceConfig& config);
#endif

/// Свободные слоты ресурсов: индекс 0 — «нет ресурса», освобождённые индексы переиспользуются.
template<typename T>
class SlotTable {
public:
    SlotTable() { m_items.emplace_back(); }

    std::uint32_t add(T item) {
        if (!m_free.empty()) {
            const std::uint32_t index = m_free.back();
            m_free.pop_back();
            m_items[index] = std::move(item);
            m_alive[index] = true;
            return index;
        }
        m_items.push_back(std::move(item));
        m_alive.resize(m_items.size(), false);
        m_alive.back() = true;
        return static_cast<std::uint32_t>(m_items.size() - 1);
    }

    [[nodiscard]] bool contains(std::uint32_t index) const noexcept {
        return index != 0 && index < m_items.size() && m_alive[index];
    }
    [[nodiscard]] T& operator[](std::uint32_t index) noexcept { return m_items[index]; }
    [[nodiscard]] const T& operator[](std::uint32_t index) const noexcept { return m_items[index]; }

    void remove(std::uint32_t index) {
        m_items[index] = T{};
        m_alive[index] = false;
        m_free.push_back(index);
    }

    template<typename Fn>
    void for_each(Fn&& fn) {
        for (std::uint32_t i = 1; i < m_items.size(); ++i) {
            if (m_alive[i]) fn(i, m_items[i]);
        }
    }

private:
    std::vector<T> m_items;
    std::vector<bool> m_alive{false};
    std::vector<std::uint32_t> m_free;
};

/// Макросы общего GLSL (см. RHI::ShaderSource) для каждого бэкенда.
inline constexpr std::string_view glsl_prelude_opengl = R"(#version 330 core
#define FLUX_OPENGL 1
#define FLUX_LOCATION(n) layout(location = n)
#define FLUX_VARYING(n)
#define FLUX_UNIFORM(s, b) layout(std140) uniform
#define FLUX_SAMPLER(s, b) uniform
#define FLUX_POSITION(p) gl_Position = (p)
#line 1
)";

inline constexpr std::string_view glsl_prelude_vulkan = R"(#version 450
#define FLUX_VULKAN 1
#define FLUX_LOCATION(n) layout(location = n)
#define FLUX_VARYING(n) layout(location = n)
#define FLUX_UNIFORM(s, b) layout(set = s, binding = b, std140) uniform
#define FLUX_SAMPLER(s, b) layout(set = s, binding = b) uniform
#define FLUX_POSITION(p) gl_Position = (p); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5
#line 1
)";

} // namespace RendererSystem::RHI
