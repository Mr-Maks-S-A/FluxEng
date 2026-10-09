/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки WindowSystem: стоимость рассылки событий и пути события через окно (Headless, без дисплея).
 *
 * Окно ОС и драйвер здесь не участвуют: замеряется то, что модуль делает сам на каждый кадр.
 * (Состояние ввода и ActionMap замеряет InputSystem.)
 */

#include <WindowSystem/Listeners.hpp>
#include <WindowSystem/Window.hpp>

#include <benchmark/benchmark.h>

namespace {

using namespace InputSystem;

void BM_Listeners_Emit(benchmark::State& state) {
    WindowSystem::Listeners<Key, Transition> listeners;
    int sink = 0;
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        listeners.subscribe([&sink](Key key, Transition t) { sink += static_cast<int>(key) + static_cast<int>(t); });
    }
    for (auto _ : state) {
        listeners.emit(Key::W, Transition::Press);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Listeners_Emit)->Arg(1)->Arg(8)->Arg(64);

WindowSystem::Window headless() {
    auto created = WindowSystem::Window::create({.backend = WindowSystem::WindowBackend::Headless});
    return std::move(*created);
}

// Путь события через окно: input().apply + typed-подписчики + общая подписка. Аргумент — число подписчиков на key.
void BM_Window_InjectKey(benchmark::State& state) {
    WindowSystem::Window window = headless();
    int sink = 0;
    for (std::int64_t i = 0; i < state.range(0); ++i) window.events().key.subscribe([&sink](Key, Transition) { ++sink; });
    window.events().input.subscribe([&sink](const InputEvent&) { ++sink; });
    for (auto _ : state) {
        window.inject_key(Key::W, Transition::Press);
        window.inject_key(Key::W, Transition::Release);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Window_InjectKey)->Arg(0)->Arg(4);

// Кадр окна: poll_events + 16 внедрённых событий + чтение состояния.
void BM_Window_Frame16Events(benchmark::State& state) {
    WindowSystem::Window window = headless();
    for (auto _ : state) {
        window.poll_events();
        for (int i = 0; i < 8; ++i) {
            window.inject_key(static_cast<Key>(4 + i), Transition::Press);
            window.inject_key(static_cast<Key>(4 + i), Transition::Release);
        }
        benchmark::DoNotOptimize(window.input().pressed(Key::D));
    }
    state.SetItemsProcessed(state.iterations() * 16);
}
BENCHMARK(BM_Window_Frame16Events);

} // namespace

BENCHMARK_MAIN();
