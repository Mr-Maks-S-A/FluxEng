#pragma once
/**
 * @file Shader.hpp
 * @brief RAII-обёртка шейдерной программы OpenGL.
 */

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace RendererSystem::GL {

/**
 * @brief Шейдерная программа (вершинный + фрагментный шейдер).
 *
 * Создаётся только через from_source() / from_files(): объект всегда валиден,
 * а ошибка компиляции возвращается текстом (с логом драйвера), а не печатается в консоль.
 *
 * Положения uniform-переменных кэшируются; поиск по `std::string_view` без аллокаций.
 *
 * @pre Все методы требуют текущий OpenGL-контекст (3.3 core).
 */
class Shader {
public:
    /**
     * @brief Компилирует и линкует программу из исходников GLSL.
     * @return Программа или текст ошибки (стадия + лог драйвера).
     */
    [[nodiscard]] static std::expected<Shader, std::string> from_source(std::string_view vertex_source,
                                                                        std::string_view fragment_source);

    /**
     * @brief Читает исходники из файлов и компилирует.
     * @return Программа или текст ошибки.
     */
    [[nodiscard]] static std::expected<Shader, std::string> from_files(const std::filesystem::path& vertex_path,
                                                                       const std::filesystem::path& fragment_path);

    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    /// @brief Делает программу текущей.
    void use() const noexcept;

    /// @brief Идентификатор программы OpenGL.
    [[nodiscard]] std::uint32_t id() const noexcept { return m_program; }

    /// @brief Положение uniform-переменной (-1, если её нет или компилятор её выбросил).
    [[nodiscard]] int uniform_location(std::string_view name) const;

    /// @name Установка uniform-переменных. Программа должна быть текущей (use()).
    /// Отсутствующие uniform игнорируются, как и в самом OpenGL.
    /// @{
    void set(std::string_view name, int value) const;
    void set(std::string_view name, float value) const;
    void set(std::string_view name, const glm::vec2& value) const;
    void set(std::string_view name, const glm::vec3& value) const;
    void set(std::string_view name, const glm::vec4& value) const;
    void set(std::string_view name, const glm::mat4& value) const;
    /// @}

private:
    explicit Shader(std::uint32_t program) noexcept : m_program(program) {}

    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };

    std::uint32_t m_program = 0;
    mutable std::unordered_map<std::string, int, StringHash, std::equal_to<>> m_uniforms;
};

} // namespace RendererSystem::GL
