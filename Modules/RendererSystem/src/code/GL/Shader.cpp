#include <RendererSystem/GL/Shader.hpp>

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace RendererSystem::GL {

namespace {

std::expected<GLuint, std::string> compile_stage(GLenum type, std::string_view source, std::string_view stage) {
    const GLuint shader = glCreateShader(type);
    const char* data = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &data, &length);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) {
        return shader;
    }

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(log_length > 0 ? log_length : 1), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    glDeleteShader(shader);
    return std::unexpected(std::format("{} shader failed to compile:\n{}", stage, log.c_str()));
}

std::expected<std::string, std::string> read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("cannot open shader file '{}'", path.string()));
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

std::expected<Shader, std::string> Shader::from_source(std::string_view vertex_source,
                                                      std::string_view fragment_source) {
    if (glCreateShader == nullptr) {
        return std::unexpected(std::string("OpenGL functions are not loaded (is there a current context?)"));
    }

    auto vertex = compile_stage(GL_VERTEX_SHADER, vertex_source, "vertex");
    if (!vertex) {
        return std::unexpected(vertex.error());
    }
    auto fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source, "fragment");
    if (!fragment) {
        glDeleteShader(*vertex);
        return std::unexpected(fragment.error());
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, *vertex);
    glAttachShader(program, *fragment);
    glLinkProgram(program);
    glDetachShader(program, *vertex);
    glDetachShader(program, *fragment);
    glDeleteShader(*vertex);
    glDeleteShader(*fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint log_length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(static_cast<std::size_t>(log_length > 0 ? log_length : 1), '\0');
        glGetProgramInfoLog(program, log_length, nullptr, log.data());
        glDeleteProgram(program);
        return std::unexpected(std::format("shader program failed to link:\n{}", log.c_str()));
    }
    return Shader(program);
}

std::expected<Shader, std::string> Shader::from_files(const std::filesystem::path& vertex_path,
                                                     const std::filesystem::path& fragment_path) {
    auto vertex = read_text(vertex_path);
    if (!vertex) {
        return std::unexpected(vertex.error());
    }
    auto fragment = read_text(fragment_path);
    if (!fragment) {
        return std::unexpected(fragment.error());
    }
    return from_source(*vertex, *fragment);
}

Shader::~Shader() {
    if (m_program != 0) {
        glDeleteProgram(m_program);
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_program(std::exchange(other.m_program, 0)), m_uniforms(std::move(other.m_uniforms)) {}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_program != 0) {
            glDeleteProgram(m_program);
        }
        m_program = std::exchange(other.m_program, 0);
        m_uniforms = std::move(other.m_uniforms);
    }
    return *this;
}

void Shader::use() const noexcept {
    glUseProgram(m_program);
}

int Shader::uniform_location(std::string_view name) const {
    if (const auto it = m_uniforms.find(name); it != m_uniforms.end()) {
        return it->second;
    }
    std::string key(name);
    const GLint location = glGetUniformLocation(m_program, key.c_str());
    m_uniforms.emplace(std::move(key), location);
    return location;
}

void Shader::set(std::string_view name, int value) const {
    glUniform1i(uniform_location(name), value);
}

void Shader::set(std::string_view name, float value) const {
    glUniform1f(uniform_location(name), value);
}

void Shader::set(std::string_view name, const glm::vec2& value) const {
    glUniform2fv(uniform_location(name), 1, glm::value_ptr(value));
}

void Shader::set(std::string_view name, const glm::vec3& value) const {
    glUniform3fv(uniform_location(name), 1, glm::value_ptr(value));
}

void Shader::set(std::string_view name, const glm::vec4& value) const {
    glUniform4fv(uniform_location(name), 1, glm::value_ptr(value));
}

void Shader::set(std::string_view name, const glm::mat4& value) const {
    glUniformMatrix4fv(uniform_location(name), 1, GL_FALSE, glm::value_ptr(value));
}

} // namespace RendererSystem::GL
