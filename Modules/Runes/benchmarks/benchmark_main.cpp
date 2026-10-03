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

BENCHMARK_MAIN();
