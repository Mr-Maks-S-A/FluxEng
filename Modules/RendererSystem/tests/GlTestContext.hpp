#pragma once
/**
 * @file GlTestContext.hpp
 * @brief Скрытое окно с OpenGL 3.3 core контекстом для GPU-тестов и бенчмарков.
 *
 * Контекст создаётся один раз на процесс. Если создать его нельзя
 * (сервер без дисплея, CI без GPU), GPU-тесты пропускаются, а CPU-тесты работают.
 */

#include <glad/glad.h>
// glad должен идти раньше GLFW
#include <GLFW/glfw3.h>

namespace RendererTests {

class GlTestContext {
public:
    /// @brief Единственный экземпляр; контекст создаётся при первом вызове.
    static GlTestContext& instance() {
        static GlTestContext context;
        return context;
    }

    /// @brief `true`, если OpenGL 3.3 доступен и контекст текущий.
    [[nodiscard]] bool available() const noexcept { return m_available; }

    GlTestContext(const GlTestContext&) = delete;
    GlTestContext& operator=(const GlTestContext&) = delete;

    // Деструктора нет намеренно: glfwTerminate() из статического деструктора на Wayland
    // (libdecor/GTK) иногда зависает, а выгрузка драйвера до отчёта LeakSanitizer
    // превращает его внутренние аллокации в «утечки неизвестного модуля».
    // Окно и контекст освобождает ОС при завершении процесса.
    ~GlTestContext() = default;

private:
    GlTestContext() {
#if defined(GLFW_PLATFORM_X11)
        // На Linux предпочитаем X11 (в том числе XWayland): скрытое окно без декораций GTK.
        if (glfwPlatformSupported(GLFW_PLATFORM_X11) == GLFW_TRUE) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        }
#endif
        m_glfw_initialized = glfwInit() == GLFW_TRUE;
        if (!m_glfw_initialized) {
            return;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        m_window = glfwCreateWindow(64, 64, "RendererSystem tests", nullptr, nullptr);
        if (m_window == nullptr) {
            return;
        }
        glfwMakeContextCurrent(m_window);
        m_available = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
    }

    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_available = false;
};

} // namespace RendererTests

/// Пропускает текущий тест, если GPU недоступен.
#define REQUIRE_GL_CONTEXT()                                                     \
    do {                                                                         \
        if (!::RendererTests::GlTestContext::instance().available()) {           \
            MESSAGE("OpenGL 3.3 context is not available — GPU test skipped");  \
            return;                                                              \
        }                                                                        \
    } while (false)
