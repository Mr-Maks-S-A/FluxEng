#include <iostream>
#include <glad/glad.h>
#include <WindowSystem/Window.hpp>
#include <RendererSystem/Shader.hpp>

int main() {
    // 1. Инициализация окна (800x600, видимое)
    Window window(800, 600, "FluxEng - Triangle Window", true);

    // 2. Вертексы треугольника: X, Y, R, G, B
    float vertices[] = {
        // Позиции          // Цвета
        -0.5f, -0.5f, 0.0f,  1.0f, 0.0f, 0.0f, // Левый нижний (Красный)
         0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f, // Правый нижний (Зеленый)
         0.0f,  0.5f, 0.0f,  0.0f, 0.0f, 1.0f  // Верхний (Синий)
    };

    // 3. Создание VAO и VBO в OpenGL
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // Атрибут 0: Позиция (3 float)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Атрибут 1: Цвет (3 float)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // 4. Загрузка и компиляция шейдеров
    std::string vertexShaderSource = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aColor;

        out vec3 ourColor;

        void main() {
            gl_Position = vec4(aPos, 1.0);
            ourColor = aColor;
        }
    )";

    std::string fragmentShaderSource = R"(
        #version 330 core
        out vec4 FragColor;
        in vec3 ourColor;

        void main() {
            FragColor = vec4(ourColor, 1.0);
        }
    )";

    Renderer::Shader shader;
    if (!shader.loadFromSource(vertexShaderSource, fragmentShaderSource)) {
        std::cerr << "Failed to load shaders!" << std::endl;
        return -1;
    }

    // 5. Главный цикл отрисовки
    while (!window.shouldClose()) {
        // Очистка экрана (темно-серый фон)
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Отрисовка треугольника
        shader.use();
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Обновление окна: свап буферов + pollEvents внутри
        window.update();
    }

    // 6. Очистка ресурсов
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    return 0;
}