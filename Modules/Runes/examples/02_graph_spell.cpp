/**
 * @example 02_graph_spell.cpp
 * Граф рун: то же заклинание, но узлами и рёбрами (основа визуального редактора). Граф компилируется в тот же
 * байт-код, что и текст; его можно сохранить в файл, прочитать обратно и развернуть из байт-кода.
 *
 * Модель: выражения (PUSH, ADD, TARGET…) дают значения по рёбрам `inputs`; операторы (CARVE, JMP_IF, HALT) идут цепочкой `next`.
 */

#include <Runes/Runes.hpp>

#include <cstdio>

using namespace Runes;
using Math::Fixed;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    // 1. Собираем граф «вырезать шар радиуса 2 в точке прицела». x, y — положение узла в редакторе (на смысл не влияют).
    Graph graph;
    const NodeId target = graph.add(Rune::Target, 0, 40, 20);
    const NodeId radius = graph.add(Rune::Push, Fixed::from_int(2).raw, 40, 90);
    const NodeId carve = graph.add(Rune::Carve, 0, 220, 50);
    const NodeId halt = graph.add(Rune::Halt, 0, 400, 50);
    graph.set_input(carve, 0, target); // вход 0: вектор центра
    graph.set_input(carve, 1, radius); // вход 1: радиус
    graph.set_next(carve, halt);       // порядок выполнения
    graph.entry = carve;

    // 2. Компиляция в байт-код — тот же, что даёт разбор текста.
    const auto program = compile(graph, "carve");
    EXPECT(program.has_value());
    std::printf("1. байт-код из графа:\n%s", disassemble(*program).c_str());
    const auto from_text = parse_program("TARGET\nPUSH 2\nCARVE\nHALT\n");
    EXPECT(from_text.has_value() && from_text->code.size() == program->code.size());

    // 3. Файл графа: текст по узлу на строку. Читается и пишется без потерь (в том числе положения узлов).
    const std::string file = serialize(graph);
    std::printf("2. файл .rungraph:\n%s", file.c_str());
    const auto back = parse_graph(file);
    EXPECT(back.has_value() && *back == graph);

    // 4. Обратная сборка: из байт-кода — граф (если значения не живут на стеке между операторами).
    const auto text = parse_program("CASTER\nPUSH 1\nPUSH 2\nADD\nPUSH 0.5\nMUL\nRAISE\n");
    const auto rebuilt = decompile(*text);
    EXPECT(rebuilt.has_value());
    std::printf("3. из байт-кода «поднять землю» получен граф из %zu узлов\n", rebuilt->nodes().size());
    const auto counter = parse_program("PUSH 3\nagain:\nPUSH -1\nADD\nDUP\nJMP_IF again\nHALT\n"); // счётчик живёт на стеке
    const auto unsupported = decompile(*counter);
    EXPECT(!unsupported.has_value() && unsupported.error().code == Code::StackNotEmpty);
    std::printf("   счётчик на стеке в граф не разворачивается: %s\n", unsupported.error().message().c_str());

    // 5. Ошибки графа — коды с указанием узла (редактор подсветит его). Текст берёт Diagnostic::message().
    Graph wrong;
    const NodeId only_vector = wrong.add(Rune::Target);
    const NodeId bad_carve = wrong.add(Rune::Carve);
    wrong.set_input(bad_carve, 0, only_vector);
    wrong.set_input(bad_carve, 1, only_vector); // радиус — вектором: нельзя
    wrong.entry = bad_carve;
    const auto error = compile(wrong);
    EXPECT(!error.has_value());
    std::printf("4. ошибка: код %.*s, узел %u — %s\n", static_cast<int>(code_name(error.error().code).size()), code_name(error.error().code).data(), error.error().node,
                error.error().message().c_str());
    EXPECT(error.error().code == Code::WrongInputType && error.error().node == bad_carve);
    std::printf("OK\n");
    return 0;
}
