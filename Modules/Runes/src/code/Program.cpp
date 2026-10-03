#include <Runes/Program.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>

namespace Runes {

namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(Rune::Count)> names = {
    "PUSH", "DUP", "DROP", "ADD", "MUL", "CASTER", "AIM", "TARGET", "MANA_AT", "JMP_IF", "HALT", "DRAW", "CARVE", "RAISE"};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string upper(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

} // namespace

std::expected<std::int32_t, Diagnostic> parse_fixed(std::string_view text) {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) negative = text.front() == '-', text.remove_prefix(1);
    if (text.empty()) return std::unexpected(Diagnostic{Code::BadNumber});
    std::int64_t integer = 0, fraction = 0, scale = 1;
    std::size_t i = 0;
    for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
        integer = integer * 10 + (text[i] - '0');
        if (integer > 32767) return std::unexpected(Diagnostic{Code::NumberOutOfRange});
    }
    if (i < text.size() && text[i] == '.') {
        for (++i; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
            if (scale < 100000) fraction = fraction * 10 + (text[i] - '0'), scale *= 10; // лишние знаки отбрасываются
        }
    }
    if (i != text.size()) return std::unexpected(Diagnostic{Code::BadNumber, 0, no_node, std::string(text)});
    const std::int64_t raw = integer * Math::Fixed::one_raw + fraction * Math::Fixed::one_raw / scale;
    return static_cast<std::int32_t>(negative ? -raw : raw);
}

std::string format_fixed(std::int32_t raw) {
    const bool negative = raw < 0;
    std::int64_t v = negative ? -static_cast<std::int64_t>(raw) : raw;
    std::string out = (negative ? "-" : "") + std::to_string(v / Math::Fixed::one_raw);
    const std::int64_t fraction = v % Math::Fixed::one_raw * 100000 / Math::Fixed::one_raw;
    if (fraction != 0) {
        std::string digits = std::to_string(fraction);
        digits.insert(0, 5 - digits.size(), '0');
        while (digits.back() == '0') digits.pop_back();
        out += '.' + digits;
    }
    return out;
}

namespace {

struct Pending {
    Instruction instruction;
    std::string label; ///< Для JMP_IF с меткой.
    int line = 0;
};

} // namespace

std::string_view rune_name(Rune rune) noexcept {
    const auto i = static_cast<std::size_t>(rune);
    return i < names.size() ? names[i] : "?";
}

std::expected<Program, Diagnostic> parse_program(std::string_view text, std::string name) {
    std::vector<Pending> pending;
    std::map<std::string, std::int32_t> labels;
    int line_number = 0;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_number;
        for (const std::string_view mark : {"#", ";", "//"}) {
            if (const std::size_t at = line.find(mark); at != std::string_view::npos) line = line.substr(0, at);
        }
        line = trim(line);
        if (line.empty()) continue;

        if (line.back() == ':') { // метка на отдельной строке
            const std::string label = upper(trim(line.substr(0, line.size() - 1)));
            if (label.empty() || labels.contains(label)) return std::unexpected(Diagnostic{Code::BadLabel, line_number});
            labels[label] = static_cast<std::int32_t>(pending.size());
            continue;
        }
        const std::size_t space = line.find_first_of(" \t");
        const std::string mnemonic = upper(line.substr(0, space));
        const std::string_view argument = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));
        const auto found = std::ranges::find(names, mnemonic);
        if (found == names.end()) return std::unexpected(Diagnostic{Code::UnknownRune, line_number, no_node, mnemonic});
        Pending p;
        p.line = line_number;
        p.instruction.rune = static_cast<Rune>(found - names.begin());
        if (has_operand(p.instruction.rune)) {
            if (argument.empty()) return std::unexpected(Diagnostic{Code::MissingOperand, line_number, no_node, mnemonic});
            if (p.instruction.rune == Rune::Push) {
                auto value = parse_fixed(argument);
                if (!value) return std::unexpected(Diagnostic{value.error().code, line_number, no_node, value.error().detail});
                p.instruction.operand = *value;
            } else if (std::isdigit(static_cast<unsigned char>(argument.front()))) {
                std::int32_t target = 0;
                const auto [end, ec] = std::from_chars(argument.data(), argument.data() + argument.size(), target);
                if (ec != std::errc{} || end != argument.data() + argument.size()) return std::unexpected(Diagnostic{Code::BadJumpTarget, line_number, no_node, std::string(argument)});
                p.instruction.operand = target;
            } else {
                p.label = upper(argument);
            }
        } else if (!argument.empty()) {
            return std::unexpected(Diagnostic{Code::UnexpectedOperand, line_number, no_node, mnemonic});
        }
        pending.push_back(std::move(p));
        if (pending.size() > max_program_length) return std::unexpected(Diagnostic{Code::ProgramTooLong, line_number});
    }
    if (pending.empty()) return std::unexpected(Diagnostic{Code::ProgramEmpty});

    Program program;
    program.name = std::move(name);
    program.code.reserve(pending.size());
    for (Pending& p : pending) {
        if (!p.label.empty()) {
            const auto it = labels.find(p.label);
            if (it == labels.end()) return std::unexpected(Diagnostic{Code::UnknownLabel, p.line, no_node, p.label});
            p.instruction.operand = it->second;
        }
        if (p.instruction.rune == Rune::JmpIf && (p.instruction.operand < 0 || static_cast<std::size_t>(p.instruction.operand) >= pending.size())) {
            return std::unexpected(Diagnostic{Code::JumpOutOfRange, p.line});
        }
        program.code.push_back(p.instruction);
    }
    return program;
}

std::string disassemble(const Program& program) {
    std::string out;
    for (const Instruction& i : program.code) {
        out += rune_name(i.rune);
        if (i.rune == Rune::Push) out += ' ' + std::to_string(Math::Fixed::from_raw(i.operand).to_double());
        if (i.rune == Rune::JmpIf) out += ' ' + std::to_string(i.operand);
        out += '\n';
    }
    return out;
}

} // namespace Runes
