// Renderer/Shader.h
#pragma once

#include <glad/glad.h>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>  // Опционально, для математики

namespace Renderer {

    /**
     * @brief Класс для управления шейдерами OpenGL
     * Компилирует, линкует и активирует шейдерные программы
     */
    class Shader {
    private:
        unsigned int m_programID;
        std::unordered_map<std::string, int> m_uniformCache;

        // Получение местоположения uniform-переменной с кэшированием
        int getUniformLocation(const std::string& name);
        
        // Проверка статуса компиляции/линковки
        bool checkCompileErrors(unsigned int shader, const std::string& type);
        bool checkLinkErrors(unsigned int program);

    public:
        Shader();
        ~Shader();

        // Запрещаем копирование
        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;

        // Разрешаем перемещение
        Shader(Shader&& other) noexcept;
        Shader& operator=(Shader&& other) noexcept;

        /**
         * @brief Загрузка шейдеров из файлов
         * @param vertexPath Путь к вершинному шейдеру
         * @param fragmentPath Путь к фрагментному шейдеру
         * @return true если успешно
         */
        bool loadFromFile(const std::string& vertexPath, const std::string& fragmentPath);

        /**
         * @brief Загрузка шейдеров из строк
         * @param vertexSource Код вершинного шейдера
         * @param fragmentSource Код фрагментного шейдера
         * @return true если успешно
         */
        bool loadFromSource(const std::string& vertexSource, const std::string& fragmentSource);

        /**
         * @brief Активация шейдерной программы
         */
        void use() const;

        /**
         * @brief Деактивация
         */
        void unuse() const;

        // Установка uniform-переменных
        void setBool(const std::string& name, bool value);
        void setInt(const std::string& name, int value);
        void setFloat(const std::string& name, float value);
        void setVec2(const std::string& name, float x, float y);
        void setVec3(const std::string& name, float x, float y, float z);
        void setVec4(const std::string& name, float x, float y, float z, float w);
        void setMat4(const std::string& name, const float* matrix);

        /**
         * @brief Получение ID программы
         */
        unsigned int getProgramID() const { return m_programID; }
    };

} // namespace Renderer