#pragma once

#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

namespace rendering {

class Shader {
public:
    Shader();
    ~Shader();

    // Запрещаем копирование
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Разрешаем перемещение
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    // Загрузка шейдеров
    bool loadFromFile(const std::string& vertexPath, const std::string& fragmentPath);
    bool loadFromSource(const std::string& vertexSource, const std::string& fragmentSource);

    // Использование шейдера
    void use() const;
    void unuse() const;

    // Uniform-переменные
    void setUniform(const std::string& name, int value);
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, const glm::vec2& value);
    void setUniform(const std::string& name, const glm::vec3& value);
    void setUniform(const std::string& name, const glm::vec4& value);
    void setUniform(const std::string& name, const glm::mat3& value);
    void setUniform(const std::string& name, const glm::mat4& value);

    // Геттеры
    unsigned int getID() const { return m_programID; }
    bool isValid() const { return m_programID != 0; }

private:
    unsigned int m_programID;
    mutable std::unordered_map<std::string, int> m_uniformLocationCache;

    // Вспомогательные методы
    unsigned int compileShader(unsigned int type, const std::string& source);
    int getUniformLocation(const std::string& name) const;
    std::string readFile(const std::string& path);
    void checkCompileErrors(unsigned int shader, const std::string& type);
    void checkLinkErrors();
};

} // namespace rendering
