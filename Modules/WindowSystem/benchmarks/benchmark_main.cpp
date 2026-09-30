/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки WindowSystem: стоимость учёта ввода и рассылки событий (без окна).
 *
 * Окно и драйвер здесь не участвуют: замеряется то, что модуль делает сам на каждый кадр.
 */

#include <WindowSystem/Input.hpp>
#include <WindowSystem/Listeners.hpp>

#include <benchmark/benchmark.h>

namespace {

void BM_Input_Frame(benchmark::State& state) {
    const auto events = static_cast<int>(state.range(0));
    WindowSystem::InputState input;
    for (auto _ : state) {
        input.begin_frame();
        for (int i = 0; i < events; ++i) {
            input.on_key(32 + (i % 64), (i & 1) != 0 ? WindowSystem::action_release : WindowSystem::action_press);
        }
        input.on_cursor(1.0, 2.0);
        input.on_scroll(0.0, 1.0);
        benchmark::DoNotOptimize(input.pressed(40));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Input_Frame)->Arg(0)->Arg(8)->Arg(64);

void BM_Input_Query(benchmark::State& state) {
    WindowSystem::InputState input;
    input.on_key(65, WindowSystem::action_press);
    int key = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(input.down(key));
        key = (key + 1) & 511;
    }
}
BENCHMARK(BM_Input_Query);

void BM_Listeners_Emit(benchmark::State& state) {
    WindowSystem::Listeners<int, int> listeners;
    int sink = 0;
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        listeners.subscribe([&sink](int key, int action) { sink += key + action; });
    }
    for (auto _ : state) {
        listeners.emit(65, 1);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Listeners_Emit)->Arg(1)->Arg(8)->Arg(64);

} // namespace

BENCHMARK_MAIN();
