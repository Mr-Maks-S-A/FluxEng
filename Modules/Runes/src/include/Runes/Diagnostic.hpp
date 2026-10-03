#pragma once
/**
 * @file Diagnostic.hpp
 * @brief Единый формат ошибок языка рун: разбор текста, графы, обратная сборка, файлы графов.
 *
 * Ошибка — это **код** (`Code`), место (строка текста, номер руны или узел графа) и короткая подробность
 * (мнемоника, имя метки…). Логика и тесты сравнивают коды; человекочитаемый текст строит `message()` в одном месте —
 * его можно заменить (другой язык, интерфейс редактора), не трогая разборщики.
 */

#include <cstdint>
#include <string>
#include <string_view>

namespace Runes {

using NodeId = std::uint32_t;
constexpr NodeId no_node = 0; ///< Идентификаторы узлов графа начинаются с 1.

enum class Code : std::uint8_t {
    // разбор текста программы
    UnknownRune,       ///< detail: мнемоника.
    MissingOperand,    ///< detail: мнемоника.
    UnexpectedOperand, ///< detail: мнемоника.
    BadNumber,         ///< detail: исходный текст (пусто — число отсутствует).
    NumberOutOfRange,
    BadLabel,          ///< Пустая или повторная метка.
    UnknownLabel,      ///< detail: имя метки.
    BadJumpTarget,     ///< detail: текст операнда.
    JumpOutOfRange,    ///< Цель JMP_IF вне программы.
    ProgramTooLong,    ///< detail: число рун (если известно).
    ProgramEmpty,
    // граф: компиляция
    NoEntry,
    MissingNode,       ///< Ссылка на несуществующий узел.
    StatementAsValue,  ///< Оператор подключён как значение; detail: руна.
    ValueInControlFlow,///< Значение стоит в цепочке управления; detail: руна.
    StackRune,         ///< DUP/DROP в графе; detail: руна.
    WrongInputCount,   ///< detail: «РУНА: нужно входов N, подключено M».
    InputNotConnected, ///< detail: номер входа.
    WrongInputType,    ///< Число вместо вектора и наоборот; detail — описание.
    DataCycle,
    MissingBranch,     ///< JMP_IF без ветки.
    UnreachableTarget,
    // граф: обратная сборка
    BadStackShape,     ///< Аргументы на стеке не складываются в значения; detail — описание.
    DrawNotDropped,
    StackNotEmpty,     ///< Значение живёт на стеке между операторами: граф так не умеет.
    JumpToEnd,
    // файл графа
    UnknownKeyword,    ///< detail: слово.
    BadField,          ///< detail: описание.
    DuplicateNode,     ///< detail: номер.
};

/// @brief Стабильное имя кода (для логов и сериализации), например «unknown_rune».
[[nodiscard]] std::string_view code_name(Code code) noexcept;

struct Diagnostic {
    Code code = Code::ProgramEmpty;
    int line = 0;        ///< Строка текста (разбор), номер руны (обратная сборка) или строка файла графа; 0 — не применимо.
    NodeId node = no_node; ///< Узел графа (компиляция); 0 — не применимо.
    std::string detail;

    /// @brief Текст по умолчанию (русский) без места.
    [[nodiscard]] std::string message() const;
    /// @brief «строка N: …» / «узел N: …» — для отчётов о загрузке файлов.
    [[nodiscard]] std::string format() const;
    [[nodiscard]] friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

} // namespace Runes
