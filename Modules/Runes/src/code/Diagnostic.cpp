#include <Runes/Diagnostic.hpp>

namespace Runes {

std::string_view code_name(Code code) noexcept {
    switch (code) {
    case Code::UnknownRune: return "unknown_rune";
    case Code::MissingOperand: return "missing_operand";
    case Code::UnexpectedOperand: return "unexpected_operand";
    case Code::BadNumber: return "bad_number";
    case Code::NumberOutOfRange: return "number_out_of_range";
    case Code::BadLabel: return "bad_label";
    case Code::UnknownLabel: return "unknown_label";
    case Code::BadJumpTarget: return "bad_jump_target";
    case Code::JumpOutOfRange: return "jump_out_of_range";
    case Code::ProgramTooLong: return "program_too_long";
    case Code::ProgramEmpty: return "program_empty";
    case Code::NoEntry: return "no_entry";
    case Code::MissingNode: return "missing_node";
    case Code::StatementAsValue: return "statement_as_value";
    case Code::ValueInControlFlow: return "value_in_control_flow";
    case Code::StackRune: return "stack_rune";
    case Code::WrongInputCount: return "wrong_input_count";
    case Code::InputNotConnected: return "input_not_connected";
    case Code::WrongInputType: return "wrong_input_type";
    case Code::DataCycle: return "data_cycle";
    case Code::MissingBranch: return "missing_branch";
    case Code::UnreachableTarget: return "unreachable_target";
    case Code::BadStackShape: return "bad_stack_shape";
    case Code::DrawNotDropped: return "draw_not_dropped";
    case Code::StackNotEmpty: return "stack_not_empty";
    case Code::JumpToEnd: return "jump_to_end";
    case Code::UnknownKeyword: return "unknown_keyword";
    case Code::BadField: return "bad_field";
    case Code::DuplicateNode: return "duplicate_node";
    }
    return "?";
}

std::string Diagnostic::message() const {
    switch (code) {
    case Code::UnknownRune: return "неизвестная руна: " + detail;
    case Code::MissingOperand: return detail + ": нужен операнд";
    case Code::UnexpectedOperand: return detail + ": операнд не нужен";
    case Code::BadNumber: return detail.empty() ? "ожидалось число" : "не число: " + detail;
    case Code::NumberOutOfRange: return "число вне диапазона Fixed (±32767)";
    case Code::BadLabel: return "пустая или повторная метка";
    case Code::UnknownLabel: return "неизвестная метка: " + detail;
    case Code::BadJumpTarget: return "не номер руны: " + detail;
    case Code::JumpOutOfRange: return "переход за пределы программы";
    case Code::ProgramTooLong: return detail.empty() ? "программа длиннее 256 рун" : "программа длиннее 256 рун (" + detail + ")";
    case Code::ProgramEmpty: return "программа пуста";
    case Code::NoEntry: return "не задан вход графа (entry)";
    case Code::MissingNode: return "ссылка на несуществующий узел";
    case Code::StatementAsValue: return detail + " — оператор, а не значение";
    case Code::ValueInControlFlow: return detail + " стоит в цепочке управления, но это значение";
    case Code::StackRune: return detail + " — операция стека: в графе используйте повторное ребро";
    case Code::WrongInputCount: return detail;
    case Code::InputNotConnected: return "вход " + detail + " не подключён";
    case Code::WrongInputType: return detail;
    case Code::DataCycle: return "цикл по данным (значение зависит от самого себя)";
    case Code::MissingBranch: return "JMP_IF без ветки branch";
    case Code::UnreachableTarget: return "цель перехода не собрана (не оператор?)";
    case Code::BadStackShape: return detail;
    case Code::DrawNotDropped: return "результат DRAW должен сниматься DROP (в графе значение не живёт между операторами)";
    case Code::StackNotEmpty: return "на стеке остались значения: граф их не выражает";
    case Code::JumpToEnd: return "JMP_IF ведёт в конец программы без операторов";
    case Code::UnknownKeyword: return "неизвестная строка: " + detail;
    case Code::BadField: return detail;
    case Code::DuplicateNode: return "повторный номер узла " + detail;
    }
    return {};
}

std::string Diagnostic::format() const {
    std::string where;
    if (node != no_node) where = "узел " + std::to_string(node) + ": ";
    else if (line > 0) where = "строка " + std::to_string(line) + ": ";
    return where + message();
}

} // namespace Runes
