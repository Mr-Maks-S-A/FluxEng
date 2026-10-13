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
 *
 * Переменные и подпрограммы (добавлены позже, в конец списка рун — старые программы и записи не меняются):
 * @code
 * PUSH 3
 * STORE 0         # переменная 0 := 3 (слотов 8: 0…7)
 * loop:
 *   LOAD 0
 *   PUSH 1
 *   SUB           # x − y
 *   DUP
 *   STORE 0
 *   PUSH 0
 *   LT            # 0 < x ? 1 : 0
 *   JMP_IF loop
 * CALL finish     # подпрограмма: возврат по RET (глубина вызовов 8)
 * HALT
 * finish:
 *   TARGET
 *   PUSH 2
 *   CARVE
 *   RET           # без вызова — то же, что HALT
 * @endcode
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
    // добавлены позже: номера прежних рун неизменны (формат Wire, записи и сеть)
    Sub, Lt,            ///< a − b; a < b → 1, иначе 0.
    Store, Load,        ///< Переменные: операнд — слот 0…7.
    Jmp, Call, Ret,     ///< Безусловный переход, вызов подпрограммы, возврат.
    Count,
};

constexpr int variable_count = 8; ///< Слотов переменных у заклинания.

[[nodiscard]] std::string_view rune_name(Rune rune) noexcept;
/// @brief У этих рун операнд — цель перехода (номер руны): JMP_IF, JMP, CALL.
[[nodiscard]] constexpr bool is_jump(Rune rune) noexcept { return rune == Rune::JmpIf || rune == Rune::Jmp || rune == Rune::Call; }
/// @brief У этих рун есть операнд: число (PUSH), цель перехода (JMP_IF, JMP, CALL) или слот переменной (STORE, LOAD).
[[nodiscard]] constexpr bool has_operand(Rune rune) noexcept { return rune == Rune::Push || is_jump(rune) || rune == Rune::Store || rune == Rune::Load; }
/// @brief Руну можно нарисовать узлом графа. Переходы и подпрограммы живут только в тексте и байт-коде.
[[nodiscard]] constexpr bool graph_expressible(Rune rune) noexcept { return rune != Rune::Jmp && rune != Rune::Call && rune != Rune::Ret && rune != Rune::Count; }

struct Instruction {
    Rune rune = Rune::Halt;
    std::int32_t operand = 0; ///< PUSH: Fixed.raw; JMP_IF/JMP/CALL: номер руны; STORE/LOAD: слот.
    [[nodiscard]] friend constexpr bool operator==(const Instruction&, const Instruction&) noexcept = default;
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
