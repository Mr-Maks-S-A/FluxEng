#pragma once

#include <string>
#include <functional>

// GLAD должен быть перед GLFW
#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace rendering {

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    // Запрещаем копирование
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Основные методы
    bool shouldClose() const;
    void swapBuffers();
    void pollEvents();
    void clear(float r, float g, float b, float a);
    
    // Геттеры
    GLFWwindow* getHandle() const { return m_window; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    float getAspectRatio() const { return static_cast<float>(m_width) / m_height; }
    
    // Настройка обратных вызовов
    void setKeyCallback(std::function<void(int key, int scancode, int action, int mods)> callback);
    void setMouseCallback(std::function<void(double xpos, double ypos)> callback);
    void setScrollCallback(std::function<void(double xoffset, double yoffset)> callback);

private:
    GLFWwindow* m_window;
    int m_width;
    int m_height;
    std::string m_title;

    // Статические колбэки для GLFW
    static void keyCallbackStatic(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseCallbackStatic(GLFWwindow* window, double xpos, double ypos);
    static void scrollCallbackStatic(GLFWwindow* window, double xoffset, double yoffset);
    
    // Пользовательские колбэки
    std::function<void(int, int, int, int)> m_keyCallback;
    std::function<void(double, double)> m_mouseCallback;
    std::function<void(double, double)> m_scrollCallback;
};

} // namespace rendering
