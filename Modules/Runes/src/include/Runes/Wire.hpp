#pragma once
/**
 * @file Wire.hpp
 * @brief Программа как байты: компактный двоичный вид для записи повтора и сети, и хеш, по которому на неё ссылаются.
 *
 * ```
 *   редактор ── compile ──► Program ── encode_program ──► байты ── content_hash ──► хеш (в команде SetProgram, 16 байт)
 *                                                          └────────── блоб в записи / пакет по сети ──────────┘
 *   приёмник: байты ── decode_program (проверка!) ──► Program
 * ```
 *
 * Формат (каноничный — одной программе соответствуют ровно одни байты, поэтому хеш однозначен):
 * `u16 число рун` (1…256), затем на каждую руну `u8 руна` и `i32 операнд` (little-endian). Имени в байтах нет:
 * имя — дело получателя. Операнд у рун без операнда обязан быть нулём.
 *
 * `decode_program` — граница доверия: байты приходят из файла или сети, поэтому проверяется всё (длина, известные руны,
 * цели переходов, нулевые операнды, отсутствие лишних байтов). Испорченное отвергается диагностикой, а не падением.
 */

#include <Runes/Program.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Runes {

/// @brief Каноничные байты программы. Программа должна быть корректной (длина 1…256): `FLUX_ASSERT` иначе.
[[nodiscard]] std::vector<std::byte> encode_program(const Program& program);

/// @brief Разбор и полная проверка байтов. Имя программы пустое.
[[nodiscard]] std::expected<Program, Diagnostic> decode_program(std::span<const std::byte> bytes);

/// @brief Хеш содержимого программы (по каноничным байтам; имя не входит): имя заклинания в сети и в записи.
[[nodiscard]] std::uint64_t program_hash(const Program& program);

/// @brief Имя программы в библиотеке по хешу: «#» и 16 шестнадцатеричных цифр.
[[nodiscard]] std::string program_name_for_hash(std::uint64_t hash);

} // namespace Runes
