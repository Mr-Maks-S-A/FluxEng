#include "Window.hpp"
#include <iostream>

namespace rendering {

Window::Window(int width, int height, const std::string& title)
    : m_width(width)
    , m_height(height)
    , m_title(title)
    , m_window(nullptr)
{
    // Инициализация GLFW
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    // Настройки OpenGL
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Создание окна
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_window);
    
    // Включение VSync
    glfwSwapInterval(1);

    // Инициализация GLAD
    if (!gladLoadGL(glfwGetProcAddress)) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
        throw std::runtime_error("Failed to initialize GLAD");
    }

    // Установка указателя на этот экземпляр для колбэков
    glfwSetWindowUserPointer(m_window, this);

    // Настройка viewport
    glViewport(0, 0, width, height);

    // Включение прозрачности
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::cout << "✅ Window created: " << width << "x" << height << std::endl;
    std::cout << "   OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
}

Window::~Window() {
    if (m_window) {
        glfwDestroyWindow(m_window);
    }
    glfwTerminate();
    std::cout << "✅ Window destroyed" << std::endl;
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void Window::swapBuffers() {
    glfwSwapBuffers(m_window);
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::clear(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Window::setKeyCallback(std::function<void(int, int, int, int)> callback) {
    m_keyCallback = callback;
    glfwSetKeyCallback(m_window, keyCallbackStatic);
}

void Window::setMouseCallback(std::function<void(double, double)> callback) {
    m_mouseCallback = callback;
    glfwSetCursorPosCallback(m_window, mouseCallbackStatic);
}

void Window::setScrollCallback(std::function<void(double, double)> callback) {
    m_scrollCallback = callback;
    glfwSetScrollCallback(m_window, scrollCallbackStatic);
}

// Статические колбэки
void Window::keyCallbackStatic(GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (win && win->m_keyCallback) {
        win->m_keyCallback(key, scancode, action, mods);
    }
}

void Window::mouseCallbackStatic(GLFWwindow* window, double xpos, double ypos) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (win && win->m_mouseCallback) {
        win->m_mouseCallback(xpos, ypos);
    }
}

void Window::scrollCallbackStatic(GLFWwindow* window, double xoffset, double yoffset) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (win && win->m_scrollCallback) {
        win->m_scrollCallback(xoffset, yoffset);
    }
}

} // namespace rendering
