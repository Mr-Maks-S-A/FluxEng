#include <Runes/Runes.hpp>

#include <doctest/doctest.h>

using namespace Runes;

namespace {
Program parse(const char* text) {
    auto p = parse_program(text);
    REQUIRE(p.has_value());
    return std::move(*p);
}
} // namespace

TEST_CASE("программа ↔ байты: туда-обратно без потерь, каноничный вид") {
    const Program p = parse("TARGET\nPUSH 2.5\nCARVE\nloop:\nPUSH 1\nJMP_IF loop\nHALT\n");
    const auto bytes = encode_program(p);
    CHECK(bytes.size() == 2 + p.code.size() * 5);
    const auto back = decode_program(bytes);
    REQUIRE(back.has_value());
    CHECK(back->code.size() == p.code.size());
    for (std::size_t i = 0; i < p.code.size(); ++i) {
        CHECK(back->code[i].rune == p.code[i].rune);
        CHECK(back->code[i].operand == p.code[i].operand);
    }
    CHECK(encode_program(*back) == bytes);
}

TEST_CASE("хеш: зависит от содержимого, не от имени") {
    Program a = parse("PUSH 1\nHALT\n");
    Program b = a;
    a.name = "first";
    b.name = "second";
    CHECK(program_hash(a) == program_hash(b));
    CHECK(program_hash(a) != program_hash(parse("PUSH 2\nHALT\n")));
    CHECK(program_name_for_hash(0xABC) == "#0000000000000abc");
}

TEST_CASE("decode_program: граница доверия — всё лишнее отвергается диагностикой") {
    const auto good = encode_program(parse("PUSH 1\nJMP_IF 0\nHALT\n"));

    CHECK(decode_program({}).error().code == Code::BadField);                              // нет заголовка
    auto truncated = good;
    truncated.pop_back();
    CHECK(decode_program(truncated).error().code == Code::BadField);                       // обрезано
    auto extra = good;
    extra.push_back(std::byte{0});
    CHECK(decode_program(extra).error().code == Code::BadField);                           // лишние байты
    auto unknown = good;
    unknown[2] = std::byte{200};
    CHECK(decode_program(unknown).error().code == Code::UnknownRune);                      // неизвестная руна
    auto operand = encode_program(parse("HALT\n"));
    operand[3] = std::byte{1};
    CHECK(decode_program(operand).error().code == Code::UnexpectedOperand);                // операнд у руны без операнда
    auto jump = good;
    jump[2 + 5 + 1] = std::byte{99};
    CHECK(decode_program(jump).error().code == Code::JumpOutOfRange);                      // прыжок за пределы
    const std::byte zero_count[] = {std::byte{0}, std::byte{0}};
    CHECK(decode_program(zero_count).error().code == Code::ProgramEmpty);
    const std::byte huge[] = {std::byte{0xFF}, std::byte{0xFF}};
    CHECK(decode_program(huge).error().code == Code::ProgramTooLong);
}

TEST_CASE("программа из графа проходит через байты и исполняется так же") {
    Graph g;
    const NodeId target = g.add(Rune::Target), radius = g.add(Rune::Push, Math::Fixed::from_int(2).raw), carve = g.add(Rune::Carve);
    g.set_input(carve, 0, target);
    g.set_input(carve, 1, radius);
    g.entry = carve;
    const auto compiled = compile(g, "g");
    REQUIRE(compiled.has_value());
    const auto back = decode_program(encode_program(*compiled));
    REQUIRE(back.has_value());
    CHECK(disassemble(*back) == disassemble(Program{"", compiled->code}));
}
