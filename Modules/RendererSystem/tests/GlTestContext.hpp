#pragma once
/**
 * @file GlTestContext.hpp
 * @brief Скрытое окно с OpenGL 3.3 core контекстом для GPU-тестов и бенчмарков.
 *
 * Контекст создаётся один раз на процесс. Если создать его нельзя
 * (сервер без дисплея, CI без GPU), GPU-тесты пропускаются, а CPU-тесты работают.
 */

#include <glad/glad.h>
#include <SDL3/SDL.h>

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

    /// @brief Снова сделать контекст текущим (если между тестами его сменили).
    void make_current() const noexcept {
        if (m_window != nullptr) SDL_GL_MakeCurrent(m_window, m_context);
    }

    GlTestContext(const GlTestContext&) = delete;
    GlTestContext& operator=(const GlTestContext&) = delete;

    // Деструктора нет намеренно: выгрузка драйвера до отчёта LeakSanitizer превращает его внутренние аллокации
    // в «утечки неизвестного модуля». Окно и контекст освобождает ОС при завершении процесса.
    ~GlTestContext() = default;

private:
    GlTestContext() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            return;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        m_window = SDL_CreateWindow("RendererSystem tests", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (m_window == nullptr) {
            return;
        }
        m_context = SDL_GL_CreateContext(m_window);
        if (m_context == nullptr) {
            return;
        }
        SDL_GL_MakeCurrent(m_window, m_context);
        m_available = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) != 0;
    }

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_context = nullptr;
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
