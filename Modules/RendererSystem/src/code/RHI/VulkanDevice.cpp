/**
 * @file VulkanDevice.cpp
 * @brief Бэкенд RHI на Vulkan 1.3: dynamic rendering, synchronization2, шейдеры — shaderc во время работы.
 *
 * Устройство намеренно простое (это набросок, а не AAA-рендер):
 * - **один кадр в полёте**: begin_frame() ждёт прошлый кадр; поэтому кольцо uniform и отложенное удаление
 *   освобождаются целиком в начале кадра;
 * - **память**: по одному выделению на ресурс (следующий шаг — VMA или свой распределитель);
 *   буферы — в видимой процессору памяти (на картах с ReBAR это и видеопамять);
 * - **кольцо кадра**: блоки uniform и Stream-буферы пишутся в большие буферы кадра с динамическими смещениями;
 * - **буфер, который уже рисовался в этом кадре**, при обновлении не перезаписывается, а заменяется новым
 *   (старый удаляется, когда GPU его дочитает) — тот же смысл, что orphaning в OpenGL;
 * - **проходы ленивые**: цель привязывается сразу, а vkCmdBeginRendering — при первом рисовании или очистке;
 *   очистка до рисования превращается в loadOp = CLEAR;
 * - **ось Y**: отрицательная высота viewport'а — тот же NDC, что в OpenGL; глубину −1…1 переводит FLUX_POSITION.
 */

#include "Backends.hpp"

#include <RendererSystem/Core/Error.hpp>

#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <vector>

// Структуры Vulkan заполняются назначенными инициализаторами; пропущенные поля (pNext, flags…) — нули по замыслу.
#if defined(__clang__)
#    pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#elif defined(__GNUC__)
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace RendererSystem::RHI {

namespace {

constexpr VkDeviceSize ring_chunk_bytes = VkDeviceSize{8} << 20;
constexpr VkFormat texture_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

std::string vk_result(VkResult result) {
    switch (result) {
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return std::format("VkResult {}", static_cast<int>(result));
    }
}

#define FLUX_VK(expr)                                                                                  \
    do {                                                                                               \
        const VkResult flux_result_ = (expr);                                                          \
        if (flux_result_ != VK_SUCCESS) throw RendererError(std::format("{}: {}", #expr, vk_result(flux_result_))); \
    } while (false)

VkFormat attribute_format(const VertexAttribute& a) {
    if (a.type == AttributeType::UnsignedByteNorm) {
        constexpr VkFormat formats[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        return formats[std::clamp<std::uint32_t>(a.components, 1, 4) - 1];
    }
    constexpr VkFormat formats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    return formats[std::clamp<std::uint32_t>(a.components, 1, 4) - 1];
}

/// Барьер «всё до → всё после» со сменой раскладки: просто и правильно; тонкая настройка — потом.
void transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                std::uint32_t base_mip = 0, std::uint32_t mips = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, base_mip, mips, 0, 1};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dependency);
}

std::uint32_t mip_count(int width, int height) {
    return static_cast<std::uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
}

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(const DeviceConfig& config) { initialize(config); }

    ~VulkanDevice() override {
        if (m_device == VK_NULL_HANDLE) {
            destroy_instance();
            return;
        }
        vkDeviceWaitIdle(m_device);
        run_deferred();
        m_pipelines.for_each([&](std::uint32_t, PipelineRes& p) { destroy_pipeline_now(p); });
        m_targets.for_each([&](std::uint32_t, TargetRes& t) { destroy_image(t.depth); });
        m_textures.for_each([&](std::uint32_t, TextureRes& t) { destroy_texture_now(t); });
        m_buffers.for_each([&](std::uint32_t, BufferRes& b) { destroy_buffer_now(b.buffer, b.memory); });
        for (RingChunk& chunk : m_ring) destroy_buffer_now(chunk.buffer, chunk.memory);
        destroy_texture_now(m_white);
        destroy_swapchain();
        for (auto& [key, sampler] : m_samplers) vkDestroySampler(m_device, sampler, nullptr);
        vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
        vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(m_device, m_uniform_layout, nullptr);
        vkDestroyDescriptorSetLayout(m_device, m_texture_layout, nullptr);
        vkDestroySemaphore(m_device, m_acquire_semaphore, nullptr);
        vkDestroyFence(m_device, m_fence, nullptr);
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        vkDestroyDevice(m_device, nullptr);
        destroy_instance();
    }

    std::size_t validation_messages() const noexcept override { return m_validation_messages; }

    // ------------------------------------------------------------------ буферы

    BufferId create_buffer(BufferKind kind, BufferUsage usage) override {
        return BufferId{m_buffers.add(BufferRes{.kind = kind, .usage = usage})};
    }

    void update_buffer(BufferId id, std::span<const std::byte> data) override {
        BufferRes& b = buffer_res(id);
        b.size = data.size();
        m_stats.uploaded_bytes += data.size();
        if (data.empty()) return;
        if (b.usage == BufferUsage::Stream) {
            b.shadow.assign(data.begin(), data.end()); // в кольцо — при рисовании (или прямо сейчас, если кадр идёт)
            b.ring_serial = 0;
            return;
        }
        const bool busy = b.used_serial == m_serial;
        if (b.buffer == VK_NULL_HANDLE || data.size() > b.capacity || busy) {
            if (b.buffer != VK_NULL_HANDLE) defer_buffer(b.buffer, b.memory);
            b.capacity = std::max<VkDeviceSize>(data.size(), 256);
            create_host_buffer(b.capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, b.buffer, b.memory, b.mapped);
            b.used_serial = 0;
        }
        std::memcpy(b.mapped, data.data(), data.size());
    }

    void destroy_buffer(BufferId id) override {
        if (!m_buffers.contains(id.index)) return;
        BufferRes& b = m_buffers[id.index];
        if (b.buffer != VK_NULL_HANDLE) defer_buffer(b.buffer, b.memory);
        m_buffers.remove(id.index);
    }

    // ------------------------------------------------------------------ текстуры

    TextureId create_texture(int width, int height, const TextureDesc& desc, std::span<const Color> pixels) override {
        if (width <= 0 || height <= 0) throw RendererError(std::format("Texture: invalid size {}x{}", width, height));
        if (!pixels.empty() && pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
            throw RendererError("Texture: pixel count does not match size");
        }
        TextureRes t = make_texture(width, height, desc, false);
        if (!pixels.empty()) {
            upload_pixels(t, pixels);
        } else {
            immediate([&](VkCommandBuffer cmd) {
                transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
            t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        return TextureId{m_textures.add(std::move(t))};
    }

    void update_texture(TextureId id, std::span<const Color> pixels) override {
        TextureRes& t = texture_res(id);
        if (pixels.size() != static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height)) {
            throw RendererError("Texture::update: pixel count does not match size");
        }
        upload_pixels(t, pixels);
    }

    void generate_mipmaps(TextureId id) override {
        TextureRes& t = texture_res(id);
        if (t.mips <= 1) return;
        if (m_recording) {
            finish_pass();
            record_mipmaps(m_cmd, t);
        } else {
            immediate([&](VkCommandBuffer cmd) { record_mipmaps(cmd, t); });
        }
    }

    void destroy_texture(TextureId id) override {
        if (!m_textures.contains(id.index) || m_textures[id.index].owned_by_target) return;
        defer_texture(m_textures[id.index]);
        m_textures.remove(id.index);
    }

    // ------------------------------------------------------------------ цели

    std::expected<TargetId, std::string> create_target(int width, int height, const TargetDesc& desc) override {
        if (width <= 0 || height <= 0) return std::unexpected(std::format("invalid render target size {}x{}", width, height));
        TextureRes color = make_texture(width, height, desc.color, true);
        immediate([&](VkCommandBuffer cmd) {
            transition(cmd, color.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        color.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        color.owned_by_target = true;
        TargetRes target{.color = TextureId{m_textures.add(std::move(color))}, .width = width, .height = height};
        if (desc.depth) target.depth = make_depth(width, height);
        return TargetId{m_targets.add(std::move(target))};
    }

    TextureId target_texture(TargetId id) const override {
        if (!m_targets.contains(id.index)) throw RendererError("Vulkan device: unknown render target");
        return m_targets[id.index].color;
    }

    void destroy_target(TargetId id) override {
        if (!m_targets.contains(id.index)) return;
        if (m_target == id.index) {
            finish_pass();
            m_target = 0;
        }
        TargetRes& t = m_targets[id.index];
        TextureRes& color = m_textures[t.color.index];
        defer_texture(color);
        m_textures.remove(t.color.index);
        if (t.depth.image != VK_NULL_HANDLE) {
            ImageRes depth = t.depth;
            m_deferred.push_back([this, depth]() mutable { destroy_image(depth); });
        }
        m_targets.remove(id.index);
    }

    UvRect target_uv() const noexcept override { return UvRect{}; } // строка 0 — верх, как у Image

    // ------------------------------------------------------------------ конвейеры

    std::expected<PipelineId, std::string> create_pipeline(const PipelineDesc& desc) override {
        PipelineRes p{.desc = desc};
        auto vertex = compile(desc.name, desc.shader.vertex, shaderc_vertex_shader);
        if (!vertex) return std::unexpected(vertex.error());
        auto fragment = compile(desc.name, desc.shader.fragment, shaderc_fragment_shader);
        if (!fragment) {
            vkDestroyShaderModule(m_device, *vertex, nullptr);
            return std::unexpected(fragment.error());
        }
        p.vertex = *vertex;
        p.fragment = *fragment;
        p.desc.shader = {}; // строки исходников не живут дольше вызова
        return PipelineId{m_pipelines.add(std::move(p))};
    }

    void destroy_pipeline(PipelineId id) override {
        if (!m_pipelines.contains(id.index)) return;
        PipelineRes p = m_pipelines[id.index];
        m_deferred.push_back([this, p]() mutable { destroy_pipeline_now(p); });
        m_pipelines.remove(id.index);
        m_bound_pipeline = VK_NULL_HANDLE;
    }

    // ------------------------------------------------------------------ кадр

    bool begin_frame(int width, int height) override {
        wait_for_gpu();
        ++m_frame_counter; // кольцо кадра начинается заново: Stream-буферы перезальются при рисовании
        m_ring_chunk = 0;
        m_ring_offset = 0;
        m_stats = {};
        m_in_frame = true;
        m_target = 0;
        m_viewport = {static_cast<std::uint32_t>(std::max(width, 0)), static_cast<std::uint32_t>(std::max(height, 0))};
        begin_recording();
        if (m_surface == VK_NULL_HANDLE) return true; // без окна: только цели
        if (width <= 0 || height <= 0) return false;
        if (m_swapchain == VK_NULL_HANDLE || m_swapchain_dirty || m_extent.width != static_cast<std::uint32_t>(width) ||
            m_extent.height != static_cast<std::uint32_t>(height)) {
            create_swapchain(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            const VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_acquire_semaphore, VK_NULL_HANDLE, &m_image);
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                m_acquired = true;
                m_acquire_waited = false;
                m_swapchain_dirty = result == VK_SUBOPTIMAL_KHR;
                m_viewport = m_extent;
                return true;
            }
            if (result != VK_ERROR_OUT_OF_DATE_KHR) throw RendererError("vkAcquireNextImageKHR: " + vk_result(result));
            create_swapchain(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        }
        return false;
    }

    void end_frame() override {
        if (!m_recording && !m_acquired) return;
        begin_recording();
        apply_pending_clear();
        finish_pass();
        if (m_acquired) {
            transition(m_cmd, m_swap_images[m_image], VK_IMAGE_ASPECT_COLOR_BIT, m_swap_layouts[m_image], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            m_swap_layouts[m_image] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        submit(m_acquired);
        if (m_acquired) {
            const VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                           .waitSemaphoreCount = 1,
                                           .pWaitSemaphores = &m_render_done[m_image],
                                           .swapchainCount = 1,
                                           .pSwapchains = &m_swapchain,
                                           .pImageIndices = &m_image};
            const VkResult result = vkQueuePresentKHR(m_queue, &present);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
                m_swapchain_dirty = true;
            } else if (result != VK_SUCCESS) {
                throw RendererError("vkQueuePresentKHR: " + vk_result(result));
            }
            m_acquired = false;
        }
        if (m_surface == VK_NULL_HANDLE) wait_for_gpu(); // без окна — синхронно: тестам проще
        m_in_frame = false;
    }

    void bind_target(TargetId target) override {
        if (target.valid() && !m_targets.contains(target.index)) throw RendererError("Vulkan device: unknown render target");
        apply_pending_clear();
        finish_pass();
        m_target = target.index;
        if (target.valid()) {
            const TargetRes& t = m_targets[target.index];
            m_viewport = {static_cast<std::uint32_t>(t.width), static_cast<std::uint32_t>(t.height)};
        } else {
            m_viewport = m_extent;
        }
        ++m_stats.passes;
    }

    void set_viewport(int width, int height) override {
        m_viewport = {static_cast<std::uint32_t>(std::max(width, 1)), static_cast<std::uint32_t>(std::max(height, 1))};
        if (m_pass_open) apply_viewport();
    }

    void clear(std::optional<Color> color, bool depth) override {
        begin_recording();
        if (m_pass_open) { // посреди прохода — очистка вложений
            std::array<VkClearAttachment, 2> attachments{};
            std::uint32_t count = 0;
            if (color) {
                const glm::vec4 c = color->to_vec4();
                attachments[count++] = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                                        .clearValue = {.color = {{c.r, c.g, c.b, c.a}}}};
            }
            if (depth && current_has_depth()) {
                attachments[count++] = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue = {.depthStencil = {1.0f, 0}}};
            }
            const VkClearRect rect{.rect = {{0, 0}, current_extent()}, .baseArrayLayer = 0, .layerCount = 1};
            if (count > 0) vkCmdClearAttachments(m_cmd, count, attachments.data(), 1, &rect);
            return;
        }
        if (color) m_pending_color = color;
        m_pending_depth = m_pending_depth || depth;
    }

    UniformSlice push_uniforms(std::span<const std::byte> data) override {
        if (data.empty()) return {};
        const VkDeviceSize align = std::max<VkDeviceSize>(m_properties.limits.minUniformBufferOffsetAlignment, 16);
        // Дескриптор блока кадра читает max_frame_uniforms байт от смещения: оставляем запас до конца буфера.
        const auto [chunk, offset] = ring_allocate(std::max<VkDeviceSize>(data.size(), max_frame_uniforms), align);
        std::memcpy(m_ring[chunk].mapped + offset, data.data(), data.size());
        m_stats.uploaded_bytes += data.size();
        return UniformSlice{static_cast<std::uint32_t>(chunk + 1), static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(data.size())};
    }

    void draw(const DrawCall& call) override {
        if (call.count == 0) return;
        if (!m_target && !m_acquired) throw RendererError("Vulkan device: nothing to draw into (no swapchain image; call begin_frame or bind a target)");
        PipelineRes& p = pipeline_res(call.pipeline);
        begin_recording();
        open_pass();

        const VkPipeline pipeline = variant(p);
        if (pipeline != m_bound_pipeline) {
            vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_bound_pipeline = pipeline;
            ++m_stats.pipeline_binds;
        }

        const auto [vertex_buffer, vertex_offset] = resolve(buffer_res(call.vertices));
        vkCmdBindVertexBuffers(m_cmd, 0, 1, &vertex_buffer, &vertex_offset);
        if (call.indices.valid()) {
            const auto [index_buffer, index_offset] = resolve(buffer_res(call.indices));
            vkCmdBindIndexBuffer(m_cmd, index_buffer, index_offset, VK_INDEX_TYPE_UINT32);
        }

        const UniformSlice frame = call.frame.valid() ? call.frame : dummy_slice();
        const UniformSlice draw_slice = !call.draw_uniforms.empty() ? push_uniforms(call.draw_uniforms) : dummy_slice();
        const TextureRes& texture = call.texture.valid() ? texture_res(call.texture) : m_white;
        const std::array<VkDescriptorSet, 3> sets{m_ring[frame.chunk - 1].frame_set, m_ring[draw_slice.chunk - 1].draw_set, texture.set};
        const std::array<std::uint32_t, 2> offsets{frame.offset, draw_slice.offset};
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 3, sets.data(), 2, offsets.data());

        if (call.indices.valid()) {
            vkCmdDrawIndexed(m_cmd, call.count, 1, call.first, 0, 0);
        } else {
            vkCmdDraw(m_cmd, call.count, 1, call.first, 0);
        }
        ++m_stats.draw_calls;
    }

    // ------------------------------------------------------------------ чтение

    Image read_target(TargetId id) override {
        if (!m_targets.contains(id.index)) throw RendererError("Vulkan device: unknown render target");
        if (m_pending_color || m_pending_depth) begin_recording();
        flush();
        const TargetRes& target = m_targets[id.index];
        TextureRes& color = m_textures[target.color.index];
        return read_image(color.image.image, color.layout, target.width, target.height, false);
    }

    Image read_screen() override {
        if (!m_acquired) throw RendererError("read_screen: no swapchain image in this frame");
        apply_pending_clear();
        finish_pass();
        flush();
        Image image = read_image(m_swap_images[m_image], m_swap_layouts[m_image], static_cast<int>(m_extent.width),
                                 static_cast<int>(m_extent.height), m_swap_format == VK_FORMAT_B8G8R8A8_UNORM);
        return image;
    }

    void wait_idle() override {
        if (m_submitted) wait_for_gpu();
        vkDeviceWaitIdle(m_device);
    }

private:
    // ------------------------------------------------------------------ ресурсы

    struct ImageRes {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct BufferRes {
        BufferKind kind = BufferKind::Vertex;
        BufferUsage usage = BufferUsage::Static;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* mapped = nullptr;
        VkDeviceSize capacity = 0;
        std::size_t size = 0;
        std::uint64_t used_serial = 0;
        // Stream: копия данных и место в кольце текущего кадра.
        std::vector<std::byte> shadow;
        std::uint64_t ring_serial = 0;
        std::size_t ring_chunk = 0;
        VkDeviceSize ring_offset = 0;
    };
    struct TextureRes {
        ImageRes image{};
        VkImageView attachment_view = VK_NULL_HANDLE; ///< Только уровень 0 — для рисования в цель с mip-уровнями.
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        int width = 0;
        int height = 0;
        std::uint32_t mips = 1;
        TextureDesc desc{};
        bool owned_by_target = false;
    };
    struct TargetRes {
        TextureId color{};
        ImageRes depth{};
        int width = 0;
        int height = 0;
    };
    struct PipelineRes {
        PipelineDesc desc{};
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        std::vector<std::pair<std::uint64_t, VkPipeline>> variants; ///< (формат цвета, есть глубина) → конвейер
    };
    struct RingChunk {
        VkDeviceSize size = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* mapped = nullptr;
        VkDescriptorSet frame_set = VK_NULL_HANDLE;
        VkDescriptorSet draw_set = VK_NULL_HANDLE;
    };

    // ------------------------------------------------------------------ инициализация

    static VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto* self = static_cast<VulkanDevice*>(user);
        if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0) {
            if (++self->m_validation_messages <= 20) std::println(stderr, "[vulkan validation] {}", data->pMessage);
        }
        return VK_FALSE;
    }

    void initialize(const DeviceConfig& config) {
        // --- экземпляр
        std::vector<const char*> extensions;
        for (const std::string& e : config.instance_extensions) extensions.push_back(e.c_str());
        std::vector<const char*> layers;
        bool validation = false;
        if (config.validation) {
            std::uint32_t count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> available(count);
            vkEnumerateInstanceLayerProperties(&count, available.data());
            validation = std::ranges::any_of(available, [](const VkLayerProperties& l) { return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
            if (validation) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            } else {
                std::println(stderr, "[vulkan] validation requested, but VK_LAYER_KHRONOS_validation is not installed");
            }
        }
        const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "FluxEng", .pEngineName = "FluxEng",
                                    .apiVersion = VK_API_VERSION_1_3};
        const VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                 .pApplicationInfo = &app,
                                                 .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
                                                 .ppEnabledLayerNames = layers.data(),
                                                 .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
                                                 .ppEnabledExtensionNames = extensions.data()};
        FLUX_VK(vkCreateInstance(&instance_info, nullptr, &m_instance));
        if (validation) {
            const VkDebugUtilsMessengerCreateInfoEXT messenger{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                .pfnUserCallback = &on_validation,
                .pUserData = this};
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create != nullptr) create(m_instance, &messenger, nullptr, &m_messenger);
        }

        // --- поверхность окна
        if (config.create_surface) {
            auto surface = config.create_surface(reinterpret_cast<std::uintptr_t>(m_instance));
            if (!surface) throw RendererError(surface.error());
            m_surface = (VkSurfaceKHR)*surface; // NOLINT: на 64 бит — указатель, на 32 — uint64_t
        }

        // --- видеокарта: Vulkan 1.3, dynamic rendering, synchronization2, очередь графики (и показа)
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
        int best_score = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_3) continue;
            VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features13};
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!features13.dynamicRendering || !features13.synchronization2) continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> family_props(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, family_props.data());
            for (std::uint32_t f = 0; f < families; ++f) {
                if ((family_props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
                if (m_surface != VK_NULL_HANDLE) {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(candidate, f, m_surface, &present);
                    if (!present) continue;
                }
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
                if (score > best_score) {
                    best_score = score;
                    m_physical = candidate;
                    m_queue_family = f;
                }
                break;
            }
        }
        if (m_physical == VK_NULL_HANDLE) throw RendererError("no Vulkan 1.3 GPU with dynamic rendering and synchronization2");
        vkGetPhysicalDeviceProperties(m_physical, &m_properties);
        vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memory);

        // --- логическое устройство
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = m_queue_family,
                                                 .queueCount = 1, .pQueuePriorities = &priority};
        VkPhysicalDeviceVulkan13Features enable13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                  .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
        std::vector<const char*> device_extensions;
        if (m_surface != VK_NULL_HANDLE) device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        const VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                             .pNext = &enable13,
                                             .queueCreateInfoCount = 1,
                                             .pQueueCreateInfos = &queue_info,
                                             .enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size()),
                                             .ppEnabledExtensionNames = device_extensions.data()};
        FLUX_VK(vkCreateDevice(m_physical, &device_info, nullptr, &m_device));
        vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

        // --- команды и синхронизация
        const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = m_queue_family};
        FLUX_VK(vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool));
        const VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = m_command_pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        FLUX_VK(vkAllocateCommandBuffers(m_device, &alloc, &m_cmd));
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        FLUX_VK(vkCreateFence(m_device, &fence_info, nullptr, &m_fence));
        const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        FLUX_VK(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_acquire_semaphore));

        // --- дескрипторы: set 0 — Frame, set 1 — Draw (динамические UBO), set 2 — текстура
        const VkDescriptorSetLayoutBinding uniform_binding{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                                           .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkDescriptorSetLayoutCreateInfo uniform_layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &uniform_binding};
        FLUX_VK(vkCreateDescriptorSetLayout(m_device, &uniform_layout, nullptr, &m_uniform_layout));
        const VkDescriptorSetLayoutBinding texture_binding{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                           .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkDescriptorSetLayoutCreateInfo texture_layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &texture_binding};
        FLUX_VK(vkCreateDescriptorSetLayout(m_device, &texture_layout, nullptr, &m_texture_layout));
        const std::array<VkDescriptorSetLayout, 3> set_layouts{m_uniform_layout, m_uniform_layout, m_texture_layout};
        const VkPipelineLayoutCreateInfo pipeline_layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                         .setLayoutCount = 3, .pSetLayouts = set_layouts.data()};
        FLUX_VK(vkCreatePipelineLayout(m_device, &pipeline_layout, nullptr, &m_pipeline_layout));
        const std::array<VkDescriptorPoolSize, 2> sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 512},
                                                        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192}};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                              .maxSets = 8192 + 512, .poolSizeCount = 2, .pPoolSizes = sizes.data()};
        FLUX_VK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptor_pool));

        // --- белая текстура и первый буфер кольца
        const Color white = Colors::white;
        m_white = make_texture(1, 1, TextureDesc{}, false);
        upload_pixels(m_white, std::span<const Color>(&white, 1));
        add_ring_chunk();

        m_info.backend = Backend::Vulkan;
        m_info.device_name = m_properties.deviceName;
        m_info.api_version = std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(m_properties.apiVersion),
                                         VK_API_VERSION_MINOR(m_properties.apiVersion), VK_API_VERSION_PATCH(m_properties.apiVersion));
        m_info.presents = m_surface != VK_NULL_HANDLE;
        m_vsync = config.vsync;
    }

    void destroy_instance() {
        if (m_compiler != nullptr) shaderc_compiler_release(m_compiler);
        m_compiler = nullptr;
        if (m_instance == VK_NULL_HANDLE) return;
        if (m_surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        if (m_messenger != VK_NULL_HANDLE) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr) destroy(m_instance, m_messenger, nullptr);
        }
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    // ------------------------------------------------------------------ память

    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags fallback = 0) const {
        for (const VkMemoryPropertyFlags flags : {wanted, fallback}) {
            if (flags == 0) continue;
            for (std::uint32_t i = 0; i < m_memory.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) != 0 && (m_memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
            }
        }
        throw RendererError("Vulkan device: no suitable memory type");
    }

    void create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, std::byte*& mapped) {
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        FLUX_VK(vkCreateBuffer(m_device, &info, nullptr, &buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
        constexpr VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
                                         .memoryTypeIndex = memory_type(requirements.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host)};
        FLUX_VK(vkAllocateMemory(m_device, &alloc, nullptr, &memory));
        FLUX_VK(vkBindBufferMemory(m_device, buffer, memory, 0));
        void* pointer = nullptr;
        FLUX_VK(vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, &pointer));
        mapped = static_cast<std::byte*>(pointer);
    }

    void destroy_buffer_now(VkBuffer buffer, VkDeviceMemory memory) {
        if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE) vkFreeMemory(m_device, memory, nullptr);
    }

    void defer_buffer(VkBuffer buffer, VkDeviceMemory memory) {
        m_deferred.push_back([this, buffer, memory] { destroy_buffer_now(buffer, memory); });
    }

    ImageRes make_image(int width, int height, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mips) {
        ImageRes res;
        const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                     .imageType = VK_IMAGE_TYPE_2D,
                                     .format = format,
                                     .extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1},
                                     .mipLevels = mips,
                                     .arrayLayers = 1,
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = usage,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        FLUX_VK(vkCreateImage(m_device, &info, nullptr, &res.image));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, res.image, &requirements);
        const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
                                         .memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        FLUX_VK(vkAllocateMemory(m_device, &alloc, nullptr, &res.memory));
        FLUX_VK(vkBindImageMemory(m_device, res.image, res.memory, 0));
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = res.image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = format,
                                         .subresourceRange = {aspect, 0, mips, 0, 1}};
        FLUX_VK(vkCreateImageView(m_device, &view, nullptr, &res.view));
        return res;
    }

    void destroy_image(ImageRes& res) {
        if (res.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, res.view, nullptr);
        if (res.image != VK_NULL_HANDLE) vkDestroyImage(m_device, res.image, nullptr);
        if (res.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, res.memory, nullptr);
        res = {};
    }

    ImageRes make_depth(int width, int height) {
        return make_image(width, height, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    }

    VkSampler sampler_for(const TextureDesc& desc, std::uint32_t mips) {
        const std::uint32_t key = static_cast<std::uint32_t>(desc.filter) | (static_cast<std::uint32_t>(desc.wrap) << 4) | ((mips > 1 ? 1u : 0u) << 8);
        if (const auto it = m_samplers.find(key); it != m_samplers.end()) return it->second;
        const VkFilter filter = desc.filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        const VkSamplerAddressMode wrap = desc.wrap == TextureWrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .magFilter = filter,
                                       .minFilter = filter,
                                       .mipmapMode = desc.filter == TextureFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                       .addressModeU = wrap,
                                       .addressModeV = wrap,
                                       .addressModeW = wrap,
                                       .maxLod = mips > 1 ? VK_LOD_CLAMP_NONE : 0.0f};
        VkSampler sampler = VK_NULL_HANDLE;
        FLUX_VK(vkCreateSampler(m_device, &info, nullptr, &sampler));
        m_samplers.emplace(key, sampler);
        return sampler;
    }

    TextureRes make_texture(int width, int height, const TextureDesc& desc, bool attachment) {
        TextureRes t;
        t.width = width;
        t.height = height;
        t.desc = desc;
        t.mips = desc.mipmaps ? mip_count(width, height) : 1;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (attachment) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        t.image = make_image(width, height, texture_format, usage, VK_IMAGE_ASPECT_COLOR_BIT, t.mips);
        if (attachment && t.mips > 1) { // вложение рендеринга — ровно один mip-уровень
            const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t.image.image,
                                             .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = texture_format,
                                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            FLUX_VK(vkCreateImageView(m_device, &view, nullptr, &t.attachment_view));
        }

        const VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = m_descriptor_pool,
                                                .descriptorSetCount = 1, .pSetLayouts = &m_texture_layout};
        FLUX_VK(vkAllocateDescriptorSets(m_device, &alloc, &t.set));
        const VkDescriptorImageInfo image_info{.sampler = sampler_for(desc, t.mips), .imageView = t.image.view,
                                               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t.set, .dstBinding = 0,
                                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image_info};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        return t;
    }

    void destroy_texture_now(TextureRes& t) {
        if (t.attachment_view != VK_NULL_HANDLE) vkDestroyImageView(m_device, t.attachment_view, nullptr);
        t.attachment_view = VK_NULL_HANDLE;
        if (t.set != VK_NULL_HANDLE) vkFreeDescriptorSets(m_device, m_descriptor_pool, 1, &t.set);
        t.set = VK_NULL_HANDLE;
        destroy_image(t.image);
    }

    void defer_texture(TextureRes& t) {
        TextureRes copy = t;
        m_deferred.push_back([this, copy]() mutable { destroy_texture_now(copy); });
        t = {};
    }

    void upload_pixels(TextureRes& t, std::span<const Color> pixels) {
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* mapped = nullptr;
        create_host_buffer(pixels.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, memory, mapped);
        std::memcpy(mapped, pixels.data(), pixels.size_bytes());
        immediate([&](VkCommandBuffer cmd) {
            transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                           .imageExtent = {static_cast<std::uint32_t>(t.width), static_cast<std::uint32_t>(t.height), 1}};
            vkCmdCopyBufferToImage(cmd, staging, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            t.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            if (t.mips > 1) {
                record_mipmaps(cmd, t);
            } else {
                transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        });
        destroy_buffer_now(staging, memory);
        m_stats.uploaded_bytes += pixels.size_bytes();
    }

    /// Уровень 0 → уменьшенные копии; в конце все уровни читаются шейдером.
    void record_mipmaps(VkCommandBuffer cmd, TextureRes& t) {
        transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        int width = t.width;
        int height = t.height;
        for (std::uint32_t level = 1; level < t.mips; ++level) {
            transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level - 1, 1);
            const int next_width = std::max(width / 2, 1);
            const int next_height = std::max(height / 2, 1);
            const VkImageBlit blit{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
                                   .srcOffsets = {{0, 0, 0}, {width, height, 1}},
                                   .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                                   .dstOffsets = {{0, 0, 0}, {next_width, next_height, 1}}};
            vkCmdBlitImage(cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, level - 1, 1);
            width = next_width;
            height = next_height;
        }
        transition(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t.mips - 1, 1);
        t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ------------------------------------------------------------------ команды

    /// Разовая работа (загрузка текстуры, чтение): свой буфер команд, отправка и ожидание.
    void immediate(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = m_command_pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        FLUX_VK(vkAllocateCommandBuffers(m_device, &alloc, &cmd));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        FLUX_VK(vkBeginCommandBuffer(cmd, &begin));
        record(cmd);
        FLUX_VK(vkEndCommandBuffer(cmd));
        VkFence fence = VK_NULL_HANDLE;
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        FLUX_VK(vkCreateFence(m_device, &fence_info, nullptr, &fence));
        const VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
        const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info};
        FLUX_VK(vkQueueSubmit2(m_queue, 1, &submit, fence));
        FLUX_VK(vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(m_device, fence, nullptr);
        vkFreeCommandBuffers(m_device, m_command_pool, 1, &cmd);
    }

    void begin_recording() {
        if (m_recording) return;
        if (m_submitted) wait_for_gpu();
        FLUX_VK(vkResetCommandBuffer(m_cmd, 0));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        FLUX_VK(vkBeginCommandBuffer(m_cmd, &begin));
        m_recording = true;
        m_bound_pipeline = VK_NULL_HANDLE;
    }

    /// Отправляет записанное. present — сигналить семафор показа (последняя отправка кадра).
    void submit(bool present) {
        if (!m_recording) return;
        FLUX_VK(vkEndCommandBuffer(m_cmd));
        m_recording = false;
        const VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = m_cmd};
        VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = m_acquire_semaphore,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkSubmitInfo2 info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info};
        if (m_acquired && !m_acquire_waited) { // первая отправка после взятия изображения ждёт его готовности
            info.waitSemaphoreInfoCount = 1;
            info.pWaitSemaphoreInfos = &wait;
            m_acquire_waited = true;
        }
        if (present) {
            signal.semaphore = m_render_done[m_image];
            info.signalSemaphoreInfoCount = 1;
            info.pSignalSemaphoreInfos = &signal;
        }
        FLUX_VK(vkQueueSubmit2(m_queue, 1, &info, m_fence));
        m_submitted = true;
    }

    void wait_for_gpu() {
        if (m_submitted) {
            FLUX_VK(vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX));
            FLUX_VK(vkResetFences(m_device, 1, &m_fence));
            m_submitted = false;
        }
        ++m_serial; // всё, что было записано раньше, GPU прочитал
        run_deferred();
    }

    /// Посреди кадра: отправить записанное и дождаться (перед чтением пикселей). Кольцо кадра не сбрасывается.
    void flush() {
        if (!m_recording) return;
        apply_pending_clear();
        finish_pass();
        submit(false);
        wait_for_gpu();
        begin_recording();
    }

    void run_deferred() {
        auto deferred = std::move(m_deferred);
        m_deferred.clear();
        for (auto& destroy : deferred) destroy();
    }

    // ------------------------------------------------------------------ кольцо кадра

    void add_ring_chunk(VkDeviceSize size = ring_chunk_bytes) {
        RingChunk chunk;
        chunk.size = size;
        create_host_buffer(size,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           chunk.buffer, chunk.memory, chunk.mapped);
        const std::array<VkDescriptorSetLayout, 2> layouts{m_uniform_layout, m_uniform_layout};
        std::array<VkDescriptorSet, 2> sets{};
        const VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = m_descriptor_pool,
                                                .descriptorSetCount = 2, .pSetLayouts = layouts.data()};
        FLUX_VK(vkAllocateDescriptorSets(m_device, &alloc, sets.data()));
        chunk.frame_set = sets[0];
        chunk.draw_set = sets[1];
        const VkDescriptorBufferInfo frame_info{chunk.buffer, 0, max_frame_uniforms};
        const VkDescriptorBufferInfo draw_info{chunk.buffer, 0, max_draw_uniforms};
        const std::array<VkWriteDescriptorSet, 2> writes{
            VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = chunk.frame_set, .dstBinding = 0, .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &frame_info},
            VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = chunk.draw_set, .dstBinding = 0, .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &draw_info}};
        vkUpdateDescriptorSets(m_device, 2, writes.data(), 0, nullptr);
        m_ring.push_back(chunk);
    }

    /// Место в кольце кадра. Не влезает в текущий блок — следующий подходящий; нет такого — новый
    /// (обычного размера или ровно под большой Stream-буфер). Блоки переиспользуются в следующих кадрах.
    std::pair<std::size_t, VkDeviceSize> ring_allocate(VkDeviceSize size, VkDeviceSize align) {
        VkDeviceSize offset = (m_ring_offset + align - 1) / align * align;
        if (offset + size > m_ring[m_ring_chunk].size) {
            do {
                ++m_ring_chunk;
            } while (m_ring_chunk < m_ring.size() && m_ring[m_ring_chunk].size < size);
            if (m_ring_chunk >= m_ring.size()) {
                add_ring_chunk(std::max(ring_chunk_bytes, (size + 0xFFFF) & ~VkDeviceSize{0xFFFF}));
                m_ring_chunk = m_ring.size() - 1;
            }
            offset = 0;
        }
        m_ring_offset = offset + size;
        return {m_ring_chunk, offset};
    }

    UniformSlice dummy_slice() {
        if (!m_dummy.valid() || m_dummy_serial != m_frame_counter) {
            static constexpr std::array<std::byte, 16> zero{};
            m_dummy = push_uniforms(zero);
            m_dummy_serial = m_frame_counter;
        }
        return m_dummy;
    }

    std::pair<VkBuffer, VkDeviceSize> resolve(BufferRes& b) {
        if (b.usage == BufferUsage::Stream) {
            if (b.ring_serial != m_frame_counter || b.ring_serial == 0) { // данные ещё не в кольце этого кадра
                const auto [chunk, offset] = ring_allocate(std::max<VkDeviceSize>(b.shadow.size(), 4), 16);
                if (!b.shadow.empty()) std::memcpy(m_ring[chunk].mapped + offset, b.shadow.data(), b.shadow.size());
                b.ring_chunk = chunk;
                b.ring_offset = offset;
                b.ring_serial = m_frame_counter;
            }
            return {m_ring[b.ring_chunk].buffer, b.ring_offset};
        }
        if (b.buffer == VK_NULL_HANDLE) throw RendererError("Vulkan device: drawing an empty buffer");
        b.used_serial = m_serial;
        return {b.buffer, 0};
    }

    // ------------------------------------------------------------------ шейдеры и конвейеры

    std::expected<VkShaderModule, std::string> compile(const std::string& name, std::string_view body, shaderc_shader_kind kind) {
        const std::string source = std::string(glsl_prelude_vulkan) + std::string(body);
        if (m_compiler == nullptr) m_compiler = shaderc_compiler_initialize();
        shaderc_compiler_t compiler = m_compiler;
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);
        const std::string file = name.empty() ? std::string("shader") : name;
        shaderc_compilation_result_t result =
            shaderc_compile_into_spv(compiler, source.data(), source.size(), kind, file.c_str(), "main", options);
        std::expected<VkShaderModule, std::string> out = std::unexpected(std::string());
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
            out = std::unexpected(std::format("pipeline '{}': {} shader failed to compile:\n{}", name,
                                              kind == shaderc_vertex_shader ? "vertex" : "fragment", shaderc_result_get_error_message(result)));
        } else {
            const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = shaderc_result_get_length(result),
                                                .pCode = reinterpret_cast<const std::uint32_t*>(shaderc_result_get_bytes(result))};
            VkShaderModule module = VK_NULL_HANDLE;
            FLUX_VK(vkCreateShaderModule(m_device, &info, nullptr, &module));
            out = module;
        }
        shaderc_result_release(result);
        shaderc_compile_options_release(options);
        return out;
    }

    void destroy_pipeline_now(PipelineRes& p) {
        for (auto& [key, pipeline] : p.variants) vkDestroyPipeline(m_device, pipeline, nullptr);
        p.variants.clear();
        if (p.vertex != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, p.vertex, nullptr);
        if (p.fragment != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, p.fragment, nullptr);
        p.vertex = p.fragment = VK_NULL_HANDLE;
    }

    /// Конвейер под формат текущей цели (с dynamic rendering формат вложений — часть конвейера).
    VkPipeline variant(PipelineRes& p) {
        const VkFormat color = m_target ? texture_format : m_swap_format;
        const bool depth = current_has_depth();
        const std::uint64_t key = (static_cast<std::uint64_t>(color) << 1) | (depth ? 1u : 0u);
        for (const auto& [k, pipeline] : p.variants) {
            if (k == key) return pipeline;
        }
        const PipelineDesc& d = p.desc;
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
            VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p.vertex, .pName = "main"},
            VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p.fragment, .pName = "main"}};
        const VkVertexInputBindingDescription binding{0, static_cast<std::uint32_t>(d.layout.stride), VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (const VertexAttribute& a : d.layout.used()) {
            attributes.push_back({a.location, 0, attribute_format(a), static_cast<std::uint32_t>(a.offset)});
        }
        const VkPipelineVertexInputStateCreateInfo vertex_input{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                                .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
                                                                .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
                                                                .pVertexAttributeDescriptions = attributes.data()};
        const VkPipelineInputAssemblyStateCreateInfo assembly{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                              .topology = d.primitive == Primitive::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkPipelineViewportStateCreateInfo viewport{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        const VkPipelineRasterizationStateCreateInfo raster{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                            .polygonMode = VK_POLYGON_MODE_FILL,
                                                            .cullMode = d.cull == CullMode::Back ? VkCullModeFlags{VK_CULL_MODE_BACK_BIT} : VkCullModeFlags{VK_CULL_MODE_NONE},
                                                            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                            .lineWidth = 1.0f};
        const VkPipelineMultisampleStateCreateInfo multisample{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        const bool test = depth && d.depth != DepthMode::Off;
        const VkPipelineDepthStencilStateCreateInfo depth_state{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                                .depthTestEnable = test ? VK_TRUE : VK_FALSE,
                                                                .depthWriteEnable = test && d.depth == DepthMode::TestWrite ? VK_TRUE : VK_FALSE,
                                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
        VkPipelineColorBlendAttachmentState blend{.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
        if (d.blend != BlendMode::Opaque) {
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = d.blend == BlendMode::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        const VkPipelineColorBlendStateCreateInfo color_blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                              .attachmentCount = 1, .pAttachments = &blend};
        const std::array<VkDynamicState, 2> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic_state{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                             .dynamicStateCount = 2, .pDynamicStates = dynamic.data()};
        const VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                      .colorAttachmentCount = 1, .pColorAttachmentFormats = &color,
                                                      .depthAttachmentFormat = depth ? depth_format : VK_FORMAT_UNDEFINED};
        const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .pNext = &rendering,
                                                .stageCount = 2,
                                                .pStages = stages.data(),
                                                .pVertexInputState = &vertex_input,
                                                .pInputAssemblyState = &assembly,
                                                .pViewportState = &viewport,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &multisample,
                                                .pDepthStencilState = &depth_state,
                                                .pColorBlendState = &color_blend,
                                                .pDynamicState = &dynamic_state,
                                                .layout = m_pipeline_layout};
        VkPipeline pipeline = VK_NULL_HANDLE;
        FLUX_VK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        p.variants.emplace_back(key, pipeline);
        return pipeline;
    }

    // ------------------------------------------------------------------ проходы

    [[nodiscard]] bool current_has_depth() const noexcept {
        return m_target ? m_targets[m_target].depth.image != VK_NULL_HANDLE : m_swap_depth.image != VK_NULL_HANDLE;
    }

    [[nodiscard]] VkExtent2D current_extent() const noexcept {
        if (m_target) {
            const TargetRes& t = m_targets[m_target];
            return {static_cast<std::uint32_t>(t.width), static_cast<std::uint32_t>(t.height)};
        }
        return m_extent;
    }

    void apply_viewport() {
        // Отрицательная высота: ось Y как в OpenGL (NDC +1 — верх кадра).
        const VkViewport viewport{0.0f, static_cast<float>(m_viewport.height), static_cast<float>(m_viewport.width),
                                  -static_cast<float>(m_viewport.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, current_extent()};
        vkCmdSetViewport(m_cmd, 0, 1, &viewport);
        vkCmdSetScissor(m_cmd, 0, 1, &scissor);
    }

    void open_pass() {
        if (m_pass_open) return;
        begin_recording();
        VkImage color_image = VK_NULL_HANDLE;
        VkImageView color_view = VK_NULL_HANDLE;
        VkImageLayout* color_layout = nullptr;
        ImageRes* depth = nullptr;
        if (m_target) {
            TargetRes& t = m_targets[m_target];
            TextureRes& c = m_textures[t.color.index];
            color_image = c.image.image;
            color_view = c.attachment_view != VK_NULL_HANDLE ? c.attachment_view : c.image.view;
            color_layout = &c.layout;
            if (t.depth.image != VK_NULL_HANDLE) depth = &t.depth;
        } else {
            if (!m_acquired) throw RendererError("Vulkan device: no swapchain image to draw into");
            color_image = m_swap_images[m_image];
            color_view = m_swap_views[m_image];
            color_layout = &m_swap_layouts[m_image];
            depth = &m_swap_depth;
        }
        // Все mip-уровни: раскладка отслеживается для изображения целиком (рисуем в уровень 0, остальные строит generate_mipmaps).
        transition(m_cmd, color_image, VK_IMAGE_ASPECT_COLOR_BIT, *color_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        *color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (depth != nullptr) {
            transition(m_cmd, depth->image, VK_IMAGE_ASPECT_DEPTH_BIT, depth->layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            depth->layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }
        const glm::vec4 c = m_pending_color ? m_pending_color->to_vec4() : glm::vec4{0.0f};
        const VkRenderingAttachmentInfo color_attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                         .imageView = color_view,
                                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                         .loadOp = m_pending_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                         .clearValue = {.color = {{c.r, c.g, c.b, c.a}}}};
        const VkRenderingAttachmentInfo depth_attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                         .imageView = depth != nullptr ? depth->view : VK_NULL_HANDLE,
                                                         .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                                         .loadOp = m_pending_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                         .clearValue = {.depthStencil = {1.0f, 0}}};
        const VkRenderingInfo info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                   .renderArea = {{0, 0}, current_extent()},
                                   .layerCount = 1,
                                   .colorAttachmentCount = 1,
                                   .pColorAttachments = &color_attachment,
                                   .pDepthAttachment = depth != nullptr ? &depth_attachment : nullptr};
        vkCmdBeginRendering(m_cmd, &info);
        m_pass_open = true;
        m_pending_color.reset();
        m_pending_depth = false;
        m_bound_pipeline = VK_NULL_HANDLE;
        apply_viewport();
    }

    void finish_pass() {
        if (!m_pass_open) return;
        vkCmdEndRendering(m_cmd);
        m_pass_open = false;
        if (m_target) { // цель снова читается шейдерами
            TextureRes& c = m_textures[m_targets[m_target].color.index];
            transition(m_cmd, c.image.image, VK_IMAGE_ASPECT_COLOR_BIT, c.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            c.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    /// Очистка без последующего рисования всё равно должна случиться.
    void apply_pending_clear() {
        if ((m_pending_color || m_pending_depth) && !m_pass_open && (m_target || m_acquired)) {
            open_pass();
            finish_pass();
        }
        m_pending_color.reset();
        m_pending_depth = false;
    }

    Image read_image(VkImage image, VkImageLayout& layout, int width, int height, bool bgra) {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* mapped = nullptr;
        const auto bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
        create_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffer, memory, mapped);
        const VkImageLayout original = layout;
        immediate([&](VkCommandBuffer cmd) {
            transition(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, original, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
            const VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                           .imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1}};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            transition(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       original == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : original, 0, 1);
        });
        if (original == VK_IMAGE_LAYOUT_UNDEFINED) layout = VK_IMAGE_LAYOUT_GENERAL;
        Image out(width, height);
        std::memcpy(out.pixels().data(), mapped, bytes);
        destroy_buffer_now(buffer, memory);
        if (bgra) {
            for (Color& c : out.pixels()) std::swap(c.r, c.b);
        }
        return out;
    }

    // ------------------------------------------------------------------ swapchain

    void create_swapchain(std::uint32_t width, std::uint32_t height) {
        vkDeviceWaitIdle(m_device);
        VkSurfaceCapabilitiesKHR caps{};
        FLUX_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps));
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &count, formats.data());
        VkSurfaceFormatKHR chosen = formats.front();
        for (const VkSurfaceFormatKHR& f : formats) { // UNORM — те же цвета, что у OpenGL-бэкенда (без sRGB-преобразования)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &count, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!m_vsync) {
            for (const VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
                if (std::ranges::find(modes, wanted) != modes.end()) {
                    mode = wanted;
                    break;
                }
            }
        }
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
        }
        std::uint32_t images = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) images = std::min(images, caps.maxImageCount);

        const VkSwapchainKHR old = m_swapchain;
        const VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                            .surface = m_surface,
                                            .minImageCount = images,
                                            .imageFormat = chosen.format,
                                            .imageColorSpace = chosen.colorSpace,
                                            .imageExtent = extent,
                                            .imageArrayLayers = 1,
                                            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                            .preTransform = caps.currentTransform,
                                            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                            .presentMode = mode,
                                            .clipped = VK_TRUE,
                                            .oldSwapchain = old};
        FLUX_VK(vkCreateSwapchainKHR(m_device, &info, nullptr, &m_swapchain));
        destroy_swapchain_images();
        if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, old, nullptr);

        m_swap_format = chosen.format;
        m_extent = extent;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
        m_swap_images.resize(count);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_swap_images.data());
        m_swap_layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
        for (VkImage image : m_swap_images) {
            const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                             .format = m_swap_format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkImageView v = VK_NULL_HANDLE;
            FLUX_VK(vkCreateImageView(m_device, &view, nullptr, &v));
            m_swap_views.push_back(v);
            VkSemaphore s = VK_NULL_HANDLE;
            const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            FLUX_VK(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &s));
            m_render_done.push_back(s);
        }
        m_swap_depth = make_depth(static_cast<int>(extent.width), static_cast<int>(extent.height));
        m_swapchain_dirty = false;
    }

    void destroy_swapchain_images() {
        for (VkImageView v : m_swap_views) vkDestroyImageView(m_device, v, nullptr);
        for (VkSemaphore s : m_render_done) vkDestroySemaphore(m_device, s, nullptr);
        m_swap_views.clear();
        m_render_done.clear();
        m_swap_images.clear();
        destroy_image(m_swap_depth);
    }

    void destroy_swapchain() {
        destroy_swapchain_images();
        if (m_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }

    // ------------------------------------------------------------------ доступ

    BufferRes& buffer_res(BufferId id) {
        if (!m_buffers.contains(id.index)) throw RendererError(std::format("Vulkan device: unknown buffer {}", id.index));
        return m_buffers[id.index];
    }
    TextureRes& texture_res(TextureId id) {
        if (!m_textures.contains(id.index)) throw RendererError(std::format("Vulkan device: unknown texture {}", id.index));
        return m_textures[id.index];
    }
    PipelineRes& pipeline_res(PipelineId id) {
        if (!m_pipelines.contains(id.index)) throw RendererError(std::format("Vulkan device: unknown pipeline {}", id.index));
        return m_pipelines[id.index];
    }

    // ------------------------------------------------------------------ состояние

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties{};
    VkPhysicalDeviceMemoryProperties m_memory{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queue_family = 0;
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    VkSemaphore m_acquire_semaphore = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_uniform_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_texture_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    std::map<std::uint32_t, VkSampler> m_samplers;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D m_extent{0, 0};
    std::vector<VkImage> m_swap_images;
    std::vector<VkImageView> m_swap_views;
    std::vector<VkImageLayout> m_swap_layouts;
    std::vector<VkSemaphore> m_render_done;
    ImageRes m_swap_depth{};
    std::uint32_t m_image = 0;
    bool m_acquired = false;
    bool m_acquire_waited = false;
    bool m_swapchain_dirty = false;
    bool m_vsync = true;

    SlotTable<BufferRes> m_buffers;
    SlotTable<TextureRes> m_textures;
    SlotTable<TargetRes> m_targets;
    SlotTable<PipelineRes> m_pipelines;
    TextureRes m_white{};
    std::vector<RingChunk> m_ring;
    std::size_t m_ring_chunk = 0;
    VkDeviceSize m_ring_offset = 0;
    UniformSlice m_dummy{};
    std::uint64_t m_dummy_serial = 0;
    std::vector<std::function<void()>> m_deferred;

    std::uint64_t m_serial = 1;        ///< Растёт после каждого ожидания GPU.
    std::uint64_t m_frame_counter = 1; ///< Растёт в каждом begin_frame (кольцо сбрасывается).
    bool m_recording = false;
    bool m_submitted = false;
    bool m_in_frame = false;
    bool m_pass_open = false;
    std::uint32_t m_target = 0;
    VkExtent2D m_viewport{0, 0};
    std::optional<Color> m_pending_color;
    bool m_pending_depth = false;
    VkPipeline m_bound_pipeline = VK_NULL_HANDLE;
    std::size_t m_validation_messages = 0;
    shaderc_compiler_t m_compiler = nullptr;

};

} // namespace

std::expected<std::unique_ptr<Device>, std::string> make_vulkan_device(const DeviceConfig& config) {
    try {
        return std::unique_ptr<Device>(new VulkanDevice(config));
    } catch (const std::exception& error) {
        return std::unexpected(std::string(error.what()));
    }
}

} // namespace RendererSystem::RHI
