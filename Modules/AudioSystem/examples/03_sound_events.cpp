// Звуковые события: звукорежиссёр описывает «шаг по траве» текстом, программист пишет events.post("footstep", позиция).
#include <AudioSystem/AudioSystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <set>

using namespace AudioSystem;

namespace {
// std::println появится в стандартной библиотеке не у всех компиляторов; std::format есть везде, где собирается движок.
template <class... Args>
void println(std::format_string<Args...> format, Args&&... args) {
    std::puts(std::format(format, std::forward<Args>(args)...).c_str());
}
} // namespace

namespace {
constexpr const char* sound_bank = R"(
# Шаги: три клипа на выбор, без повторов подряд, лёгкий разброс громкости и высоты; не чаще раза в 80 мс.
[footstep]
clips = step_1 step_2 step_3
volume = 0.8
volume_var = 0.1
pitch_var = 1.5
spatial = true
min_distance = 2
max_distance = 30
cooldown = 0.08
max_instances = 4

# Интерфейс: без 3D, поверх всего.
[ui_click]
clips = click
bus = ui
priority = 250

# Огонь: зацикленный, затухает по расстоянию линейно, чуть реверберации.
[campfire]
clips = fire
bus = ambient
loop = true
spatial = true
model = linear
max_distance = 20
reverb_send = 0.2
)";
} // namespace

int main() {
    AudioEngine engine;
    engine.add_clip("step_1", make_noise(0.1, 48'000, 1, 0.4f));
    engine.add_clip("step_2", make_noise(0.1, 48'000, 2, 0.4f));
    engine.add_clip("step_3", make_noise(0.1, 48'000, 3, 0.4f));
    engine.add_clip("click", make_sine(1800.0f, 0.05));
    engine.add_clip("fire", make_noise(2.0, 48'000, 4, 0.2f));

    SoundEvents events(engine, /*seed=*/2024);
    const auto loaded = events.load_text(sound_bank);
    if (!loaded) {
        println("ошибка в банке звуков: {}", loaded.error());
        return EXIT_FAILURE;
    }
    println("загружено событий: {}", *loaded);

    // Опечатка в данных — понятное сообщение со строкой, а не молчаливый пропуск.
    const auto broken = events.load_text("[oops]\nclips = a\nvolum = 1\n");
    println("проверка ошибки: {}", broken ? "?!" : broken.error());

    events.post("campfire", {6.0f, 0.0f, -4.0f});
    int started = 0;
    for (int step = 0; step < 12; ++step) {
        // Игрок бежит: шаг каждые 0.12 с — реже, чем остывание в 80 мс, поэтому каждый шаг звучит.
        if (events.post("footstep", {static_cast<float>(step) * 0.5f, 0.0f, -2.0f})) ++started;
        (void)render_offline(engine, 0.12);
    }
    // Два шага в один и тот же момент (дубль события от физики): второй гасится остыванием.
    const bool first = events.post("footstep", {6.0f, 0.0f, -2.0f}).value != 0;
    const bool twin = events.post("footstep", {6.0f, 0.0f, -2.0f}).value != 0;
    events.post("ui_click");
    (void)render_offline(engine, 0.02);
    println("шагов запущено: {} из 12; дубль в тот же момент: {}", started, first && !twin ? "погашен" : "прошёл");
    println("часы звука: {:.2f} с, голосов сейчас: {}", engine.time_seconds(), engine.active_voices());

    // Текст в обе стороны: правки звукорежиссёра можно сохранять.
    SoundEvents copy(engine);
    const bool round_trip = copy.load_text(events.to_text()).has_value() && copy.to_text() == events.to_text();
    println("текст → события → текст без потерь: {}", round_trip ? "да" : "нет");

    return started == 12 && first && !twin && round_trip && engine.active_voices() >= 3 ? EXIT_SUCCESS : EXIT_FAILURE;
}
