#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <functional>
#include <string>

class Window {
public:
    // Конструктор принимает флаг видимости (true для игры, false для тестов)
    Window(int width, int height, const std::string& title, bool visible = true);
    ~Window();

    // Запрещаем копирование
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Разрешаем перемещение
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    bool shouldClose() const;
    void update();

    // Геттеры
    GLFWwindow* getNativeWindow() const { return m_window; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    // Пользовательские callbacks
    std::function<void(int key, int action)> onKeyPress;
    std::function<void(int width, int height)> onResize;

private:
    GLFWwindow* m_window = nullptr;
    int m_width;
    int m_height;
    std::string m_title;

    // Подсчет активных окон для управления glfwInit/glfwTerminate
    static inline size_t s_windowCount = 0;

    // Системная инициализация окружения (GTK suppress + GLFW init)
    static bool initSystem();
    static void terminateSystem();
    static void setEnvVar(const char* name, const char* value);

    // Статические C-callbacks для GLFW
    static void frameBufferResizeCallback(GLFWwindow* window, int width, int height);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
};