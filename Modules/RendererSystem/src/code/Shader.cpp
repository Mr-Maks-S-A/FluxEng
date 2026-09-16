// Modules/RendererSystem/src/code/Shader.cpp
#include <RendererSystem/Shader.hpp>
#include <fstream>
#include <sstream>
#include <iostream>

namespace Renderer {

Shader::Shader() : m_programID(0) {}

Shader::~Shader() {
    if (m_programID != 0) {
        glDeleteProgram(m_programID);
    }
}

Shader::Shader(Shader&& other) noexcept 
    : m_programID(other.m_programID), m_uniformCache(std::move(other.m_uniformCache)) {
    other.m_programID = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_programID != 0) {
            glDeleteProgram(m_programID);
        }
        m_programID = other.m_programID;
        m_uniformCache = std::move(other.m_uniformCache);
        other.m_programID = 0;
    }
    return *this;
}

bool Shader::loadFromFile(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string vertexCode;
    std::string fragmentCode;
    std::ifstream vShaderFile;
    std::ifstream fShaderFile;

    vShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    fShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try {
        vShaderFile.open(vertexPath);
        fShaderFile.open(fragmentPath);
        std::stringstream vShaderStream, fShaderStream;

        vShaderStream << vShaderFile.rdbuf();
        fShaderStream << fShaderFile.rdbuf();

        vShaderFile.close();
        fShaderFile.close();

        vertexCode = vShaderStream.str();
        fragmentCode = fShaderStream.str();
    } catch (const std::ifstream::failure& e) {
        std::cerr << "[Shader Error] Не удалось прочитать файлы шейдера: " << e.what() << "\n";
        return false;
    }

    return loadFromSource(vertexCode, fragmentCode);
}

bool Shader::loadFromSource(const std::string& vertexSource, const std::string& fragmentSource) {
    const char* vShaderCode = vertexSource.c_str();
    const char* fShaderCode = fragmentSource.c_str();

    // 1. Компиляция вершинного шейдера
    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    if (!checkCompileErrors(vertex, "VERTEX")) {
        glDeleteShader(vertex);
        return false;
    }

    // 2. Компиляция фрагментного шейдера
    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    if (!checkCompileErrors(fragment, "FRAGMENT")) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }

    // 3. Линковка программы
    m_programID = glCreateProgram();
    glAttachShader(m_programID, vertex);
    glAttachShader(m_programID, fragment);
    glLinkProgram(m_programID);

    bool success = checkLinkErrors(m_programID);

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    return success;
}

void Shader::use() const {
    if (m_programID != 0) {
        glUseProgram(m_programID);
    }
}

void Shader::unuse() const {
    glUseProgram(0);
}

int Shader::getUniformLocation(const std::string& name) {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) {
        return it->second;
    }

    int location = glGetUniformLocation(m_programID, name.c_str());
    if (location == -1) {
        std::cerr << "[Shader Warning] Uniform '" << name << "' не найден в шейдере!\n";
    }
    m_uniformCache[name] = location;
    return location;
}

void Shader::setBool(const std::string& name, bool value) {
    glUniform1i(getUniformLocation(name), (int)value);
}

void Shader::setInt(const std::string& name, int value) {
    glUniform1i(getUniformLocation(name), value);
}

void Shader::setFloat(const std::string& name, float value) {
    glUniform1f(getUniformLocation(name), value);
}

void Shader::setVec2(const std::string& name, float x, float y) {
    glUniform2f(getUniformLocation(name), x, y);
}

void Shader::setVec3(const std::string& name, float x, float y, float z) {
    glUniform3f(getUniformLocation(name), x, y, z);
}

void Shader::setVec4(const std::string& name, float x, float y, float z, float w) {
    glUniform4f(getUniformLocation(name), x, y, z, w);
}

void Shader::setMat4(const std::string& name, const float* matrix) {
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, matrix);
}

bool Shader::checkCompileErrors(unsigned int shader, const std::string& type) {
    int success;
    char infoLog[1024];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
        std::cerr << "[Shader Error] Ошибка компиляции (" << type << "):\n" << infoLog << "\n";
        return false;
    }
    return true;
}

bool Shader::checkLinkErrors(unsigned int program) {
    int success;
    char infoLog[1024];
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 1024, nullptr, infoLog);
        std::cerr << "[Shader Error] Ошибка линковки шейдерной программы:\n" << infoLog << "\n";
        return false;
    }
    return true;
}

} // namespace Renderer