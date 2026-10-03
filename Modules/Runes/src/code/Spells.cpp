#include <Runes/Spells.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Runes {

using Math::Fixed;
using Math::Mana;
using Math::FVec3;
using Math::WorldPos;

std::string_view failure_text(Failure failure) noexcept {
    switch (failure) {
    case Failure::None: return "ok";
    case Failure::StackUnderflow: return "stack underflow";
    case Failure::StackOverflow: return "stack overflow";
    case Failure::JumpOutOfRange: return "jump out of range";
    case Failure::OutOfMana: return "out of mana";
    case Failure::OutOfBudget: return "out of budget";
    case Failure::UnknownRune: return "unknown rune";
    }
    return "?";
}

// ------------------------------------------------------------ ProgramLibrary

std::expected<void, Diagnostic> ProgramLibrary::add_text(std::string name, std::string_view text) {
    auto program = parse_program(text, name);
    if (!program) return std::unexpected(program.error());
    m_programs[std::move(name)] = std::make_shared<const Program>(std::move(*program));
    return {};
}

ProgramLibrary::Report ProgramLibrary::load_directory(const std::filesystem::path& directory) {
    Report report;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file() && (entry.path().extension() == ".rune" || entry.path().extension() == ".rungraph")) files.push_back(entry.path());
    }
    if (ec) {
        report.errors.push_back(directory.string() + ": " + ec.message());
        return report;
    }
    std::ranges::sort(files); // порядок не влияет на результат, но отчёт стабилен
    for (const auto& path : files) {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        if (path.extension() == ".rungraph") { // граф → тот же байт-код
            auto graph = parse_graph(text.str());
            std::expected<Program, Diagnostic> program = graph ? compile(*graph, path.stem().string()) : std::unexpected(graph.error());
            if (program) {
                m_programs[path.stem().string()] = std::make_shared<const Program>(std::move(*program));
                ++report.loaded;
            } else {
                report.errors.push_back(path.filename().string() + ": " + program.error().format());
            }
        } else if (auto ok = add_text(path.stem().string(), text.str()); ok) {
            ++report.loaded;
        } else {
            report.errors.push_back(path.filename().string() + ": " + ok.error().format());
        }
    }
    return report;
}

std::shared_ptr<const Program> ProgramLibrary::find(std::string_view name) const {
    const auto it = m_programs.find(name);
    return it == m_programs.end() ? nullptr : it->second;
}

std::vector<std::string> ProgramLibrary::names() const {
    std::vector<std::string> out;
    for (const auto& [name, program] : m_programs) out.push_back(name);
    return out;
}

// --------------------------------------------------------------- SpellSystem

void SpellSystem::declare(EventSystem::EventBus& bus) {
    const EventSystem::ModuleId id = bus.declare_module("Runes")
                                         .produces<SpellFailedEvent>(EventSystem::ChannelConfig{.reserve = 16, .max_events_per_tick = 1024})
                                         .produces<SpellFinishedEvent>(EventSystem::ChannelConfig{.reserve = 16, .max_events_per_tick = 1024});
    m_failed = bus.writer<SpellFailedEvent>(id);
    m_finished = bus.writer<SpellFinishedEvent>(id);
    m_declared = true;
}

ECS::Entity SpellSystem::cast(ECS::World& world, ECS::Entity caster, std::shared_ptr<const Program> program, FVec3 aim, ManaSource source) {
    const ECS::Entity spell = world.create();
    m_trace = {};
    m_trace.program = program->name;
    m_trace.source = source;
    m_trace.valid = true;
    m_trace_spell = spell;
    world.emplace<SpellProgram>(spell, SpellProgram{std::move(program)});
    world.emplace<MachineState>(spell);
    world.emplace<SpellBudget>(spell, SpellBudget{.remaining = m_tuning.max_budget, .spent = {}, .source = source});
    world.emplace<SpellCaster>(spell, SpellCaster{caster, aim});
    return spell;
}

void SpellSystem::tick(ECS::World& world, SpellHost& host, EffectBuffer& effects) {
    for (const ECS::Entity spell : world.entities_of<MachineState>()) run(world, host, effects, spell); // по id, а не по порядку в пуле
}

namespace {

constexpr Fixed clamp_radius(Fixed r, const Tuning& t) { return Math::clamp(r, t.min_radius, t.max_radius); }

/// r³·k без промежуточного насыщения.
Mana effect_cost(Fixed radius, const Tuning& t) {
    const std::int64_t r = radius.raw;
    const std::int64_t cubed = r * r / Fixed::one_raw * r / Fixed::one_raw; // r³ в Q16.16
    return Mana(Fixed::saturate(cubed * t.effect_k.raw() / Fixed::one_raw));
}

} // namespace

void SpellSystem::run(ECS::World& world, SpellHost& host, EffectBuffer& effects, ECS::Entity spell) {
    MachineState& m = *world.get<MachineState>(spell);
    SpellBudget& budget = *world.get<SpellBudget>(spell);
    const SpellCaster& who = *world.get<SpellCaster>(spell);
    const Program& program = *world.get<SpellProgram>(spell)->program;
    const bool traced = spell == m_trace_spell;

    const auto fail = [&](Failure f) {
        m.status = Status::Failed;
        m.failure = f;
    };
    const auto push = [&](Fixed v) {
        if (m.sp >= stack_depth) return fail(Failure::StackOverflow);
        m.stack[m.sp++] = v;
    };
    const auto pop = [&]() -> Fixed {
        if (m.sp == 0) {
            fail(Failure::StackUnderflow);
            return {};
        }
        return m.stack[--m.sp];
    };
    const auto pop_vec = [&]() -> WorldPos {
        const Fixed z = pop(), y = pop(), x = pop();
        return Math::to_world({x, y, z});
    };
    const auto push_vec = [&](WorldPos p) {
        const FVec3 v = Math::to_fvec(p);
        push(v.x), push(v.y), push(v.z);
    };
    /// Оплата руны. false — остановить заклинание (причина уже записана).
    const auto pay = [&](Mana amount) -> bool {
        if (amount > budget.remaining) return fail(Failure::OutOfBudget), false;
        if (budget.source == ManaSource::Personal) {
            if (!host.take_personal(who.caster, amount)) return fail(Failure::OutOfMana), false;
        } else {
            if (!host.take_personal(who.caster, amount / Fixed::from_int(m_tuning.ambient_divisor))) return fail(Failure::OutOfMana), false;
            const Mana got = host.draw(host.position(who.caster), m_tuning.ambient_radius, amount);
            if (got < amount) return fail(Failure::OutOfMana), false;
        }
        budget.remaining -= amount;
        budget.spent += amount;
        return true;
    };

    std::size_t executed_now = 0;
    while (m.status == Status::Running && executed_now < max_runes_per_tick) {
        if (m.pc >= program.code.size()) { // дошли до конца — нормальная остановка
            m.status = Status::Halted;
            break;
        }
        const Instruction in = program.code[m.pc];
        Mana cost = m_tuning.rune_cost;
        FVec3 vec{};
        Fixed radius{};
        WorldPos at{};
        // Эффекты платят и за объём: радиус снимается со стека до оплаты, чтобы цена зависела от него.
        if (in.rune == Rune::Carve || in.rune == Rune::Raise) {
            if (m.sp < 4) {
                fail(Failure::StackUnderflow);
                break;
            }
            radius = clamp_radius(m.stack[m.sp - 1], m_tuning);
            cost += effect_cost(radius, m_tuning);
        }
        if (!pay(cost)) break; // pc остаётся на руне, которую не смогли оплатить
        const std::uint32_t this_pc = m.pc;
        ++m.pc;
        ++m.runes_executed;
        ++executed_now;

        switch (in.rune) {
        case Rune::Push: push(Fixed::from_raw(in.operand)); break;
        case Rune::Dup: {
            const Fixed top = pop();
            if (m.status == Status::Running) push(top), push(top);
            break;
        }
        case Rune::Drop: (void)pop(); break;
        case Rune::Add: {
            const Fixed b = pop(), a = pop();
            push(a + b);
            break;
        }
        case Rune::Mul: {
            const Fixed b = pop(), a = pop();
            push(a * b);
            break;
        }
        case Rune::Caster: push_vec(host.position(who.caster)); break;
        case Rune::Aim: vec = who.aim, push(vec.x), push(vec.y), push(vec.z); break;
        case Rune::Target: push_vec(host.target(who.caster, who.aim, m_tuning.target_range)); break;
        case Rune::ManaAt: {
            at = pop_vec();
            if (m.status == Status::Running) push(host.density(at).value);
            break;
        }
        case Rune::JmpIf: {
            const Fixed condition = pop();
            if (m.status != Status::Running) break;
            if (condition.raw != 0) {
                if (in.operand < 0 || static_cast<std::size_t>(in.operand) >= program.code.size()) {
                    fail(Failure::JumpOutOfRange);
                } else {
                    m.pc = static_cast<std::uint32_t>(in.operand);
                }
            }
            break;
        }
        case Rune::Halt: m.status = Status::Halted; break;
        case Rune::Draw: {
            const Mana amount(pop());
            const Fixed r = clamp_radius(pop(), m_tuning);
            at = pop_vec();
            if (m.status != Status::Running) break;
            const Mana got = host.draw(at, r, amount);
            host.give_personal(who.caster, got);
            push(got.value);
            break;
        }
        case Rune::Carve:
        case Rune::Raise: {
            (void)pop(); // радиус (уже зажат выше)
            at = pop_vec();
            if (m.status != Status::Running) break;
            effects.push_back({in.rune == Rune::Carve ? Effect::Kind::Carve : Effect::Kind::Raise, who.caster, at, radius});
            if (traced) ++m_trace.effects;
            break;
        }
        case Rune::Count: fail(Failure::UnknownRune); break;
        }
        if (traced) {
            m_trace.runes_executed = m.runes_executed;
            m_trace.spent = budget.spent;
            SpellTrace::Entry entry{this_pc, in.rune, cost};
            if (m_trace.entries.size() < SpellTrace::max_entries) m_trace.entries.push_back(entry);
            else m_trace.entries[(m.runes_executed - 1) % SpellTrace::max_entries] = entry;
        }
    }

    if (traced) {
        m_trace.status = m.status;
        m_trace.failure = m.failure;
        m_trace.failed_pc = m.pc;
        m_trace.spent = budget.spent;
    }
    if (m.status == Status::Failed) {
        if (m_declared) (void)m_failed.emit(SpellFailedEvent{who.caster.index, spell.index, static_cast<std::uint32_t>(m.failure), m.pc});
        world.destroy(spell);
    } else if (m.status == Status::Halted) {
        if (m_declared) (void)m_finished.emit(SpellFinishedEvent{who.caster.index, spell.index, m.runes_executed, budget.spent.raw()});
        world.destroy(spell);
    }
}

void hash_spells(const ECS::World& world, Math::Hasher& hasher) {
    for (const ECS::Entity spell : world.entities_of<MachineState>()) {
        const MachineState& m = *world.get<MachineState>(spell);
        const SpellBudget& b = *world.get<SpellBudget>(spell);
        hasher.add(spell.index);
        hasher.add(m.pc), hasher.add(m.sp), hasher.add(m.runes_executed), hasher.add(static_cast<std::uint64_t>(m.status));
        for (std::uint32_t i = 0; i < m.sp; ++i) hasher.add_signed(m.stack[i].raw);
        hasher.add_signed(b.remaining.raw()), hasher.add_signed(b.spent.raw());
    }
}

} // namespace Runes
