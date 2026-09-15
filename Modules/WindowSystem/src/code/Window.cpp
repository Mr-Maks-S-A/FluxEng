#include <WindowSystem/Window.hpp>
#include <iostream>
#include <cstdlib>

void Window::setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool Window::initSystem() {
    // 1. Подавляем шум GTK/GLib на Linux (на Windows автоматически игнорируется)
    setEnvVar("G_MESSAGES_DEBUG", "");
    setEnvVar("NO_AT_BRIDGE", "1");

    // 2. Инициализируем GLFW
    if (!glfwInit()) {
        std::cerr << "[WindowSystem Error] Ошибка инициализации GLFW!\n";
        return false;
    }
    return true;
}

void Window::terminateSystem() {
    glfwTerminate();
}

Window::Window(int width, int height, const std::string& title, bool visible)
    : m_width(width), m_height(height), m_title(title) 
{
    // Инициализируем GLFW только при создании ПЕРВОГО окна
    if (s_windowCount == 0) {
        if (!initSystem()) return;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Параметр видимости (нужен для тестов и скрытых окон)
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::cerr << "[WindowSystem Error] Не удалось создать окно GLFW!\n";
        return;
    }

    glfwMakeContextCurrent(m_window);

    // Инициализируем GLAD при создании первого контекста
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "[GLAD Error] Не удалось инициализировать OpenGL контекст!\n";
        return;
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, frameBufferResizeCallback);
    glfwSetKeyCallback(m_window, keyCallback);

    // Увеличиваем счетчик созданных окон
    s_windowCount++;
}

Window::~Window() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        s_windowCount--;
    }

    // Завершаем GLFW только когда ЗАКРЫТО ПОСЛЕДНЕЕ окно
    if (s_windowCount == 0) {
        terminateSystem();
    }
}

// Конструктор перемещения
Window::Window(Window&& other) noexcept 
    : m_window(other.m_window), m_width(other.m_width), 
      m_height(other.m_height), m_title(std::move(other.m_title)),
      onKeyPress(std::move(other.onKeyPress)), onResize(std::move(other.onResize))
{
    other.m_window = nullptr;
    if (m_window) {
        glfwSetWindowUserPointer(m_window, this);
    }
}

// Оператор перемещения
Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (m_window) {
            glfwDestroyWindow(m_window);
            s_windowCount--;
        }

        m_window = other.m_window;
        m_width = other.m_width;
        m_height = other.m_height;
        m_title = std::move(other.m_title);
        onKeyPress = std::move(other.onKeyPress);
        onResize = std::move(other.onResize);

        other.m_window = nullptr;
        if (m_window) {
            glfwSetWindowUserPointer(m_window, this);
        }

        if (s_windowCount == 0 && !m_window) {
            terminateSystem();
        }
    }
    return *this;
}

bool Window::shouldClose() const {
    return m_window ? glfwWindowShouldClose(m_window) : true;
}

void Window::update() {
    if (m_window) {
        glfwSwapBuffers(m_window);
        glfwPollEvents();
    }
}

void Window::frameBufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->m_width = width;
        self->m_height = height;
        glViewport(0, 0, width, height);
        
        if (self->onResize) {
            self->onResize(width, height);
        }
    }
}

void Window::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        if (self->onKeyPress) {
            self->onKeyPress(key, action);
        }
    }
}