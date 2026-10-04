#include <Runes/Wire.hpp>

#include <Math/Assert.hpp>
#include <Math/Hash.hpp>

#include <cstdio>

namespace Runes {

std::vector<std::byte> encode_program(const Program& program) {
    FLUX_ASSERT(!program.code.empty() && program.code.size() <= max_program_length, "encode_program: длина программы вне 1…256");
    std::vector<std::byte> out;
    out.reserve(2 + program.code.size() * 5);
    const auto put = [&out](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::byte>(v >> (8 * i)));
    };
    put(static_cast<std::uint32_t>(program.code.size()), 2);
    for (const Instruction& i : program.code) {
        put(static_cast<std::uint32_t>(i.rune), 1);
        put(static_cast<std::uint32_t>(i.operand), 4);
    }
    return out;
}

std::expected<Program, Diagnostic> decode_program(std::span<const std::byte> bytes) {
    const auto fail = [](Code code, std::string detail = {}) { return std::unexpected(Diagnostic{.code = code, .detail = std::move(detail)}); };
    if (bytes.size() < 2) return fail(Code::BadField, "программа: обрезан заголовок");
    const auto get = [&bytes](std::size_t at, int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + static_cast<std::size_t>(i)])) << (8 * i);
        return v;
    };
    const std::size_t count = get(0, 2);
    if (count == 0) return fail(Code::ProgramEmpty);
    if (count > max_program_length) return fail(Code::ProgramTooLong, std::to_string(count));
    if (bytes.size() != 2 + count * 5) return fail(Code::BadField, "программа: длина байтов не сходится с числом рун");

    Program program;
    program.code.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t rune = get(2 + i * 5, 1);
        const auto operand = static_cast<std::int32_t>(get(2 + i * 5 + 1, 4));
        if (rune >= static_cast<std::uint32_t>(Rune::Count)) return fail(Code::UnknownRune, std::to_string(rune));
        const auto r = static_cast<Rune>(rune);
        if (!has_operand(r) && operand != 0) return fail(Code::UnexpectedOperand, std::string(rune_name(r)));
        if (r == Rune::JmpIf && (operand < 0 || static_cast<std::size_t>(operand) >= count)) return fail(Code::JumpOutOfRange, std::to_string(operand));
        program.code.push_back({r, operand});
    }
    return program;
}

std::uint64_t program_hash(const Program& program) { return Math::content_hash(encode_program(program)); }

std::string program_name_for_hash(std::uint64_t hash) {
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "#%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

} // namespace Runes
