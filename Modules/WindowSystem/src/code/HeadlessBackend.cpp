#include <WindowSystem/Backend.hpp>

#include <algorithm>
#include <utility>

namespace WindowSystem {

namespace {

/// Окно без экрана: размеры из конфигурации, события только внедрённые через Window::inject*. Графики нет.
class HeadlessBackend final : public IWindowBackend {
public:
    explicit HeadlessBackend(const WindowConfig& config) : m_size{std::max(config.width, 0), std::max(config.height, 0)} {}

    void poll_events() override {}
    void swap_buffers() override {}
    [[nodiscard]] bool should_close() const noexcept override { return m_close; }
    void request_close() noexcept override { m_close = true; }
    void set_title(const std::string&) override {}
    [[nodiscard]] Size window_size() const noexcept override { return m_size; }
    [[nodiscard]] Size framebuffer_size() const noexcept override { return m_size; }
    [[nodiscard]] float content_scale() const noexcept override { return 1.0f; }
    void set_vsync(bool) noexcept override {}
    void set_cursor_mode(CursorMode) noexcept override {}
    [[nodiscard]] InputSystem::Vec2d cursor_position() const noexcept override { return {}; }
    [[nodiscard]] void* native_handle() const noexcept override { return nullptr; }
    [[nodiscard]] std::expected<std::uint64_t, std::string> create_vulkan_surface(std::uintptr_t) const override {
        return std::unexpected(std::string("a headless window has no surface"));
    }

private:
    Size m_size;
    bool m_close = false;
};

} // namespace

BackendResult create_headless_backend(const WindowConfig& config, IBackendSink&) {
    return std::unique_ptr<IWindowBackend>(std::make_unique<HeadlessBackend>(config));
}

} // namespace WindowSystem
