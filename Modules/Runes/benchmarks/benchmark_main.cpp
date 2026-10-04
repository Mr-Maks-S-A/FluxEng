#include <Runes/Runes.hpp>

#include <benchmark/benchmark.h>

namespace {

struct BenchHost final : Runes::SpellHost {
    Math::WorldPos position(ECS::Entity) override { return {}; }
    Math::WorldPos target(ECS::Entity, Math::FVec3, Math::Fixed) override { return {}; }
    Math::Mana density(Math::WorldPos) override { return Math::Mana::from_int(30); }
    Math::Mana draw(Math::WorldPos, Math::Fixed, Math::Mana amount) override { return amount; }
    bool take_personal(ECS::Entity, Math::Mana) override { return true; }
    void give_personal(ECS::Entity, Math::Mana) override {}
};

} // namespace

/// Цикл машины: бесконечный цикл из 4 рун исполняет ровно 256 рун за тик (предел тика).
static void BM_MachineTick(benchmark::State& state) {
    Runes::ProgramLibrary library;
    (void)library.add_text("loop", "loop:\n PUSH 1\n DUP\n DROP\n JMP_IF loop\n");
    ECS::World world;
    Runes::SpellSystem system;
    BenchHost host;
    Runes::EffectBuffer effects;
    const ECS::Entity caster = world.create();
    for (auto _ : state) {
        state.PauseTiming();
        if (system.active(world) == 0) (void)system.cast(world, caster, library.find("loop"), {}, Runes::ManaSource::Personal);
        state.ResumeTiming();
        effects.clear();
        system.tick(world, host, effects);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Runes::max_runes_per_tick));
}
BENCHMARK(BM_MachineTick)->Unit(benchmark::kMicrosecond);

static void BM_ParseProgram(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(Runes::parse_program("TARGET\nPUSH 2.5\nCARVE\nHALT\n"));
}
BENCHMARK(BM_ParseProgram)->Unit(benchmark::kMicrosecond);


// ---- Программа как байты (SetProgram: хеш + блоб) ----
namespace {
Runes::Program long_program() {
    Runes::Program p;
    for (int i = 0; i < 127; ++i) {
        p.code.push_back({Runes::Rune::Push, i});
        p.code.push_back({Runes::Rune::Drop, 0});
    }
    p.code.push_back({Runes::Rune::Halt, 0});
    return p;
}
} // namespace

/// Каноничные байты программы в 255 рун (почти предел): цена одной правки редактора на отправку.
static void BM_EncodeProgram(benchmark::State& state) {
    const Runes::Program p = long_program();
    for (auto _ : state) benchmark::DoNotOptimize(Runes::encode_program(p));
}
BENCHMARK(BM_EncodeProgram);

/// Разбор и проверка пришедших байтов (граница доверия).
static void BM_DecodeProgram(benchmark::State& state) {
    const auto bytes = Runes::encode_program(long_program());
    for (auto _ : state) benchmark::DoNotOptimize(Runes::decode_program(bytes));
}
BENCHMARK(BM_DecodeProgram);

/// Хеш программы: имя блоба в записи и в сети.
static void BM_ProgramHash(benchmark::State& state) {
    const Runes::Program p = long_program();
    for (auto _ : state) benchmark::DoNotOptimize(Runes::program_hash(p));
}
BENCHMARK(BM_ProgramHash);

BENCHMARK_MAIN();
