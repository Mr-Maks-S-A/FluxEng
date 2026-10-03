#pragma once
/**
 * @file Program.hpp
 * @brief Программа из рун: набор рун, разбор из текста, проверка при загрузке.
 *
 * Заклинание — данные, а не код C++: текстовый файл, по одной руне на строку.
 *
 * @code
 * # вырезать шар в точке прицела
 * TARGET        # три числа на стеке: x y z
 * PUSH 2        # радиус
 * CARVE
 * HALT
 *
 * loop:         # метка; JMP_IF принимает метку или номер руны
 *   PUSH 1
 *   JMP_IF loop
 * @endcode
 *
 * Комментарии — `#`, `;` или `//` до конца строки. Мнемоники не зависят от регистра.
 * Проверка при загрузке: известные руны, число операндов, цели переходов внутри программы, ≤ 256 рун.
 */

#include <Runes/Diagnostic.hpp>

#include <Math/Fixed.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Runes {

constexpr std::size_t max_program_length = 256;

enum class Rune : std::uint8_t {
    // данные
    Push, Dup, Drop,
    // арифметика
    Add, Mul,
    // контекст
    Caster, Aim, Target,
    // чувство
    ManaAt,
    // управление
    JmpIf, Halt,
    // эффекты
    Draw, Carve, Raise,
    Count,
};

[[nodiscard]] std::string_view rune_name(Rune rune) noexcept;
/// @brief У этих рун есть операнд: число (PUSH) или цель перехода (JMP_IF).
[[nodiscard]] constexpr bool has_operand(Rune rune) noexcept { return rune == Rune::Push || rune == Rune::JmpIf; }

struct Instruction {
    Rune rune = Rune::Halt;
    std::int32_t operand = 0; ///< PUSH: Fixed.raw; JMP_IF: номер руны.
};

struct Program {
    std::string name;
    std::vector<Instruction> code; ///< Неизменна после загрузки.
};

/// @brief Десятичная запись → Fixed.raw без float: «-2.75», «3», «.5» (до 5 знаков после запятой).
[[nodiscard]] std::expected<std::int32_t, Diagnostic> parse_fixed(std::string_view text);
/// @brief Fixed.raw → десятичная запись (до 5 знаков; для сохранения в файлы).
[[nodiscard]] std::string format_fixed(std::int32_t raw);

/// @brief Разбирает и проверяет текст программы.
[[nodiscard]] std::expected<Program, Diagnostic> parse_program(std::string_view text, std::string name = {});
/// @brief Текст по программе (метки не восстанавливаются: переходы — по номерам).
[[nodiscard]] std::string disassemble(const Program& program);

} // namespace Runes
