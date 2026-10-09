/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки InputSystem (google-benchmark).
 *
 * Группы:
 * 1. InputState: применение событий (поток ввода кадра), запросы клавиш.
 * 2. ActionMap: запрос по имени против запроса по ActionId; ось из клавиш и стика; карта на 8 и 128 действий.
 * 3. InputCommand: снимок раскладки «типичной игры» (10 кнопок, 4 оси).
 * 4. Имена и текст: разбор имени клавиши, сохранение и загрузка карты.
 * 5. InputLog: запись и разбор 100 000 событий.
 *
 * Для осмысленных цифр собирайте в Release.
 */

#include <InputSystem/InputSystem.hpp>

#include <benchmark/benchmark.h>

#include <string>

namespace {

using namespace InputSystem;

InputEvent mixed_event(int i) {
    switch (i % 5) {
        case 0: return KeyInput{static_cast<Key>(4 + i % 26), (i / 5) % 2 ? Transition::Release : Transition::Press, Modifiers::None};
        case 1: return CursorInput{static_cast<double>(i % 1920), static_cast<double>(i % 1080)};
        case 2: return MouseButtonInput{MouseButton::Left, (i / 5) % 2 ? Transition::Release : Transition::Press, Modifiers::None};
        case 3: return ScrollInput{0.0, 1.0};
        default: return GamepadAxisInput{0, GamepadAxis::LeftX, static_cast<float>(i % 100) / 100.0f};
    }
}

// -----------------------------------------------------------------------------
// 1. InputState
// -----------------------------------------------------------------------------

void BM_State_ApplyFrame(benchmark::State& state) {
    std::vector<InputEvent> events;
    for (int i = 0; i < state.range(0); ++i) events.push_back(mixed_event(i));
    InputState input;
    for (auto _ : state) {
        input.begin_frame();
        for (const InputEvent& e : events) input.apply(e);
        benchmark::DoNotOptimize(input.down(Key::W));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_State_ApplyFrame)->Arg(8)->Arg(64)->Arg(1000);

void BM_State_Query(benchmark::State& state) {
    InputState input;
    input.begin_frame();
    input.apply(KeyInput{Key::W, Transition::Press, Modifiers::None});
    for (auto _ : state) benchmark::DoNotOptimize(input.down(Key::W) && !input.pressed(Key::S));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_State_Query);

// -----------------------------------------------------------------------------
// 2. ActionMap
// -----------------------------------------------------------------------------

ActionMap make_map(int actions) {
    ActionMap map;
    for (int i = 0; i < actions; ++i) {
        const std::string name = "action_" + std::to_string(i);
        map.bind(name, static_cast<Key>(4 + i % 26)).bind(name, GamepadButton::A);
    }
    map.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX);
    return map;
}

void BM_ActionMap_PressedByName(benchmark::State& state) {
    const ActionMap map = make_map(static_cast<int>(state.range(0)));
    InputState input;
    input.begin_frame();
    const std::string name = "action_" + std::to_string(state.range(0) - 1); // последнее — худший случай поиска
    for (auto _ : state) benchmark::DoNotOptimize(map.pressed(name, input));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ActionMap_PressedByName)->Arg(8)->Arg(128);

void BM_ActionMap_PressedById(benchmark::State& state) {
    const ActionMap map = make_map(static_cast<int>(state.range(0)));
    InputState input;
    input.begin_frame();
    const ActionId id = action_id("action_" + std::to_string(state.range(0) - 1));
    for (auto _ : state) benchmark::DoNotOptimize(map.pressed(id, input));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ActionMap_PressedById)->Arg(8)->Arg(128);

void BM_ActionMap_AxisValue(benchmark::State& state) {
    const ActionMap map = make_map(8);
    InputState input;
    input.begin_frame();
    input.apply(GamepadConnectionInput{0, true});
    input.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 0.7f});
    input.apply(KeyInput{Key::D, Transition::Press, Modifiers::None});
    const ActionId id = action_id("move_x");
    for (auto _ : state) benchmark::DoNotOptimize(map.value(id, input));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ActionMap_AxisValue);

// -----------------------------------------------------------------------------
// 3. Команда
// -----------------------------------------------------------------------------

void BM_SampleCommand(benchmark::State& state) {
    ActionMap map;
    CommandLayout layout;
    for (int i = 0; i < 10; ++i) {
        const std::string name = "button_" + std::to_string(i);
        map.bind(name, static_cast<Key>(4 + i));
        layout.button(name);
    }
    for (int i = 0; i < 4; ++i) {
        const std::string name = "axis_" + std::to_string(i);
        map.bind_axis(name, static_cast<GamepadAxis>(i));
        layout.axis(name);
    }
    InputState input;
    input.begin_frame();
    input.apply(GamepadConnectionInput{0, true});
    input.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 0.4f});
    input.apply(KeyInput{Key::C, Transition::Press, Modifiers::None});
    for (auto _ : state) benchmark::DoNotOptimize(sample_command(map, input, layout));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SampleCommand);

// -----------------------------------------------------------------------------
// 4. Имена и текст
// -----------------------------------------------------------------------------

void BM_ParseKey(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(parse_key("RightSuper"));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseKey);

void BM_ActionMap_TextRoundTrip(benchmark::State& state) {
    const ActionMap map = make_map(32);
    const std::string text = map.to_text();
    for (auto _ : state) benchmark::DoNotOptimize(ActionMap::from_text(text));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(BM_ActionMap_TextRoundTrip);

// -----------------------------------------------------------------------------
// 5. InputLog
// -----------------------------------------------------------------------------

InputLog make_log(int count) {
    InputLog log;
    for (int i = 0; i < count; ++i) log.record(static_cast<std::uint32_t>(i / 8), mixed_event(i));
    return log;
}

void BM_Log_Encode(benchmark::State& state) {
    const InputLog log = make_log(100'000);
    std::size_t bytes = 0;
    for (auto _ : state) {
        auto encoded = log.to_bytes();
        bytes = encoded.size();
        benchmark::DoNotOptimize(encoded.data());
    }
    state.SetItemsProcessed(state.iterations() * 100'000);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}
BENCHMARK(BM_Log_Encode);

void BM_Log_Decode(benchmark::State& state) {
    const auto bytes = make_log(100'000).to_bytes();
    for (auto _ : state) benchmark::DoNotOptimize(InputLog::from_bytes(bytes));
    state.SetItemsProcessed(state.iterations() * 100'000);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes.size()));
}
BENCHMARK(BM_Log_Decode);

} // namespace

BENCHMARK_MAIN();
