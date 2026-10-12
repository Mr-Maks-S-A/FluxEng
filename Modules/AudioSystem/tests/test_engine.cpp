#include <AudioSystem/AudioSystem.hpp>

#include "TestSupport.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <thread>

using namespace AudioSystem;
using namespace test;

namespace {
constexpr float center = 0.70710678f; // моно по центру: −3 дБ на канал
constexpr std::size_t block = 256;

AudioClip ramp_clip(std::size_t frames, std::uint32_t rate = 48'000) {
    AudioClip c;
    c.sample_rate = rate;
    c.channels = 1;
    for (std::size_t i = 0; i < frames; ++i) c.samples.push_back(static_cast<float>(i) / static_cast<float>(frames));
    return c;
}
} // namespace

TEST_SUITE("AudioSystem.Engine") {

TEST_CASE("звук играет с нужной громкостью и панорамой, по окончании голос освобождается") {
    AudioEngine engine(small_config());
    const ClipId clip = engine.add_clip("dc", make_constant(0.5f, 4800)); // 0.1 с
    REQUIRE(clip);
    const VoiceId voice = engine.play(clip);
    REQUIRE(voice);
    CHECK(engine.is_playing(voice)); // ещё в очереди

    const auto out = render(engine, 2400);
    CHECK(engine.is_playing(voice));
    CHECK(engine.active_voices() == 1);
    CHECK(left(out, 2000) == doctest::Approx(0.5f * center));
    CHECK(right(out, 2000) == doctest::Approx(0.5f * center));

    render(engine, 4800);
    CHECK_FALSE(engine.is_playing(voice));
    CHECK(engine.active_voices() == 0);
    const auto tail = render(engine, 1000);
    CHECK(peak(tail) == 0.0f);
}

TEST_CASE("первый блок нарастает с нуля — старт без щелчка") {
    AudioEngine engine(small_config());
    engine.play(engine.add_clip("dc", make_constant(1.0f, 48'000)), {.volume = 0.5f});
    const auto out = render(engine, block);
    CHECK(std::abs(left(out, 0)) < 0.01f);
    CHECK(left(out, block - 1) == doctest::Approx(0.5f * center).epsilon(0.02));
    for (std::size_t i = 1; i < block; ++i) CHECK(left(out, i) >= left(out, i - 1)); // монотонный подъём
}

TEST_CASE("результат воспроизводим: те же команды — те же числа") {
    const auto run = [] {
        AudioEngine engine(small_config());
        const ClipId noise = engine.add_clip("n", make_noise(0.5, 44'100, 9));
        Spatial s;
        s.enabled = true;
        s.position = {5, 0, -3};
        s.velocity = {0, 0, 10};
        engine.set_reverb({.room_size = 0.7f, .damping = 0.4f, .wet = 0.3f});
        engine.play(noise, {.pitch = 1.3f, .loop = true, .reverb_send = 0.5f, .spatial = s});
        engine.play(noise, {.volume = 0.4f, .pan = -0.5f, .start_delay = 0.05f});
        return render(engine, 20'000);
    };
    CHECK(run() == run());
}

TEST_CASE("размер блока рендера не влияет на результат") {
    const auto run = [](std::size_t chunk) {
        AudioEngine engine(small_config());
        engine.play(engine.add_clip("s", make_sine(220.0f, 0.5)), {.loop = true});
        std::vector<float> all;
        for (std::size_t done = 0; done < 9600; done += chunk) {
            const auto part = render(engine, chunk);
            all.insert(all.end(), part.begin(), part.end());
        }
        return all;
    };
    CHECK(run(256) == run(256));
    // Команды применяются на границе внутреннего блока, а он не зависит от размера буфера устройства:
    const auto a = run(256), b = run(512);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i] == doctest::Approx(b[i]));
}

TEST_CASE("зацикленный клип играет бесконечно, а точки повтора пропускают интро") {
    AudioEngine engine(small_config());
    AudioClip clip = make_constant(0.2f, 1000);
    for (std::size_t i = 0; i < 400; ++i) clip.samples[i] = 1.0f; // интро громче
    clip.loop_begin = 400;
    const VoiceId voice = engine.play(engine.add_clip("loop", clip), {.volume = 1.0f, .loop = true});
    const auto first = render(engine, 2000);
    CHECK(engine.is_playing(voice));
    CHECK(left(first, 350) > 0.5f); // интро слышно один раз (первые 256 кадров — нарастание без щелчка)
    for (std::size_t i = 1000; i < 2000; ++i) CHECK(left(first, i) == doctest::Approx(0.2f * center)); // дальше только петля
    render(engine, 48'000);
    CHECK(engine.is_playing(voice));
}

TEST_CASE("питч 2 играет вдвое быстрее и на октаву выше") {
    AudioEngine engine(small_config());
    const ClipId clip = engine.add_clip("r", ramp_clip(9600)); // 0.2 с
    const VoiceId normal = engine.play(clip);
    render(engine, 9000);
    CHECK(engine.is_playing(normal));
    render(engine, 1000);
    CHECK_FALSE(engine.is_playing(normal));

    const VoiceId fast = engine.play(clip, {.pitch = 2.0f});
    render(engine, 4000);
    CHECK(engine.is_playing(fast));
    render(engine, 1200);
    CHECK_FALSE(engine.is_playing(fast));
}

TEST_CASE("клип другой частоты играет столько же секунд") {
    AudioEngine engine(small_config()); // 48 кГц
    const VoiceId voice = engine.play(engine.add_clip("lo", make_constant(0.5f, 4000, 8000))); // 0.5 с
    render(engine, 22'000);
    CHECK(engine.is_playing(voice));
    render(engine, 2400);
    CHECK_FALSE(engine.is_playing(voice));
}

TEST_CASE("интерполяция: тон на другой частоте остаётся чистым (без ступенек)") {
    AudioEngine engine(small_config());
    engine.play(engine.add_clip("s", make_sine(100.0f, 1.0, 11'025, 0.8f)), {.volume = 1.0f});
    const auto out = render(engine, 9600);
    float max_step = 0.0f;
    for (std::size_t i = 1000; i < 9000; ++i) max_step = std::max(max_step, std::abs(left(out, i) - left(out, i - 1)));
    // Синус 100 Гц амплитудой 0.8·0.707: максимальный шаг ≈ 2π·100/48000·0.57 ≈ 0.0075. Ступеньки без интерполяции дали бы заметно больше.
    CHECK(max_step < 0.01f);
}

TEST_CASE("стерео-клип: каналы проходят как есть, баланс убирает противоположный") {
    AudioEngine engine(small_config());
    AudioClip st;
    st.sample_rate = 48'000;
    st.channels = 2;
    for (int i = 0; i < 48'000; ++i) { st.samples.push_back(0.2f); st.samples.push_back(0.4f); }
    const ClipId clip = engine.add_clip("st", st);
    const VoiceId a = engine.play(clip);
    auto out = render(engine, 1024);
    CHECK(left(out, 1000) == doctest::Approx(0.2f));
    CHECK(right(out, 1000) == doctest::Approx(0.4f));
    engine.stop(a);
    render(engine, 1024);

    engine.play(clip, {.pan = 1.0f});
    out = render(engine, 1024);
    CHECK(left(out, 1000) == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(right(out, 1000) == doctest::Approx(0.4f));
}

TEST_CASE("шины: громкость шины, плавное затухание, mute и master") {
    AudioEngine engine(small_config());
    const ClipId clip = engine.add_clip("dc", make_constant(0.8f, 480'000));
    engine.play(clip, {.bus = Buses::music});
    auto out = render(engine, 2048);
    CHECK(left(out, 2000) == doctest::Approx(0.8f * center));

    engine.set_bus_volume(Buses::music, 0.5f);
    out = render(engine, 2048);
    CHECK(left(out, 2000) == doctest::Approx(0.4f * center));

    engine.set_master_volume(0.5f, 0.1f); // 4800 кадров
    out = render(engine, 2400);
    const float midway = left(out, 2399);
    CHECK(midway < 0.4f * center);
    CHECK(midway > 0.2f * center);
    out = render(engine, 4800);
    CHECK(left(out, 4799) == doctest::Approx(0.2f * center));

    engine.set_bus_mute(Buses::music, true);
    out = render(engine, 4800);
    CHECK(peak(out, 2000) == 0.0f);
    engine.set_bus_mute(Buses::music, false);
    out = render(engine, 4800);
    CHECK(left(out, 4000) == doctest::Approx(0.2f * center));
}

TEST_CASE("пользовательское дерево шин: громкость родителя действует на потомков, stop_bus рубит поддерево") {
    EngineConfig c = small_config();
    c.buses = {{"master", 0, 1.0f}, {"world", 0, 0.5f}, {"footsteps", 1, 1.0f}, {"ui", 0, 1.0f}};
    AudioEngine engine(c);
    const BusId footsteps = engine.find_bus("footsteps");
    const BusId ui = engine.find_bus("ui");
    CHECK(footsteps.index == 2);
    CHECK(engine.find_bus("nope").index >= engine.bus_count());

    const ClipId clip = engine.add_clip("dc", make_constant(0.5f, 480'000));
    const VoiceId step = engine.play(clip, {.bus = footsteps});
    const VoiceId click = engine.play(clip, {.bus = ui});
    auto out = render(engine, 2048);
    CHECK(left(out, 2000) == doctest::Approx((0.5f * 0.5f + 0.5f) * center)); // world 0.5 · 0.5 + ui 1.0 · 0.5

    engine.stop_bus(engine.find_bus("world"));
    render(engine, 512);
    CHECK_FALSE(engine.is_playing(step));
    CHECK(engine.is_playing(click));
}

TEST_CASE("некорректное дерево шин исправляется: родитель позже потомка → корень") {
    EngineConfig c = small_config();
    c.buses = {{"master", 5, 1.0f}, {"a", 2, 1.0f}, {"b", 0, 1.0f}};
    AudioEngine engine(c);
    CHECK(engine.config().buses[0].parent == 0);
    CHECK(engine.config().buses[1].parent == 0);
    engine.play(engine.add_clip("dc", make_constant(1.0f, 4800)), {.bus = BusId{1}});
    CHECK(peak(render(engine, 2048)) > 0.5f);
}

TEST_CASE("неверная шина в PlayParams не роняет движок") {
    AudioEngine engine(small_config());
    engine.play(engine.add_clip("dc", make_constant(1.0f, 4800)), {.bus = BusId{999}});
    engine.set_bus_volume(BusId{999}, 0.0f);
    engine.set_bus_mute(BusId{999}, true);
    CHECK(peak(render(engine, 1024)) > 0.0f);
}

TEST_CASE("приоритеты: пул занят — вытесняется самый неважный, менее важный новый отбрасывается") {
    EngineConfig c = small_config();
    c.max_voices = 2;
    AudioEngine engine(c);
    const ClipId clip = engine.add_clip("dc", make_constant(0.1f, 480'000));
    const VoiceId low = engine.play(clip, {.priority = 10});
    const VoiceId mid = engine.play(clip, {.priority = 100});
    render(engine, 256);
    CHECK(engine.active_voices() == 2);

    const VoiceId high = engine.play(clip, {.priority = 200});
    render(engine, 256);
    CHECK(engine.stats().stolen_voices == 1);
    CHECK_FALSE(engine.is_playing(low));
    CHECK(engine.is_playing(mid));
    CHECK(engine.is_playing(high));

    const VoiceId weak = engine.play(clip, {.priority = 1});
    render(engine, 256);
    CHECK(engine.stats().dropped_voices == 1);
    CHECK_FALSE(engine.is_playing(weak));
    CHECK(engine.is_playing(mid));
    CHECK(engine.is_playing(high));
}

TEST_CASE("при равных приоритетах вытесняется самый тихий, затем самый старый") {
    EngineConfig c = small_config();
    c.max_voices = 2;
    AudioEngine engine(c);
    const ClipId clip = engine.add_clip("dc", make_constant(1.0f, 480'000));
    const VoiceId loud = engine.play(clip, {.volume = 1.0f});
    const VoiceId quiet = engine.play(clip, {.volume = 0.1f});
    render(engine, 512);
    engine.play(clip);
    render(engine, 256);
    CHECK(engine.is_playing(loud));
    CHECK_FALSE(engine.is_playing(quiet));
}

TEST_CASE("пауза голоса замораживает позицию, возобновление продолжает с того же места") {
    AudioEngine engine(small_config());
    const VoiceId voice = engine.play(engine.add_clip("r", ramp_clip(48'000)), {.volume = 1.0f});
    render(engine, 4800);
    engine.pause(voice, true);
    render(engine, 512); // доводим громкость до нуля
    const auto paused = render(engine, 4800);
    CHECK(peak(paused) == 0.0f);
    CHECK(engine.is_playing(voice));
    engine.pause(voice, false);
    const auto resumed = render(engine, 1024);
    // Рампа ≈ 4800/48000 = 0.1 на момент паузы (плюс хвост 1–2 блока), а не 0.2 (если бы время шло).
    const float value = left(resumed, 1023) / center;
    CHECK(value > 0.09f);
    CHECK(value < 0.14f);
}

TEST_CASE("пауза шины замораживает все голоса и возобновляет") {
    AudioEngine engine(small_config());
    const VoiceId voice = engine.play(engine.add_clip("r", ramp_clip(48'000)), {.bus = Buses::music});
    render(engine, 2400);
    engine.pause_bus(Buses::music, true);
    render(engine, 1024);
    CHECK(peak(render(engine, 4800)) == 0.0f);
    engine.pause_bus(Buses::music, false);
    CHECK(peak(render(engine, 1024)) > 0.0f);
    CHECK(engine.is_playing(voice));
}

TEST_CASE("задержка старта, начальное смещение, fade-in") {
    AudioEngine engine(small_config());
    const ClipId dc = engine.add_clip("dc", make_constant(1.0f, 480'000));

    engine.play(dc, {.start_delay = 0.1f}); // 4800 кадров
    auto out = render(engine, 9600);
    CHECK(peak(out, 0, 4700) == 0.0f);
    CHECK(left(out, 9500) == doctest::Approx(center));

    engine.stop_all();
    render(engine, 1024);

    const ClipId r = engine.add_clip("r", ramp_clip(48'000));
    engine.play(r, {.start_offset = 0.5f});
    out = render(engine, 2048);
    CHECK(left(out, 2000) / center == doctest::Approx(0.5f).epsilon(0.05));

    engine.stop_all();
    render(engine, 1024);

    engine.play(dc, {.fade_in = 0.1f});
    out = render(engine, 9600);
    CHECK(left(out, 2400) == doctest::Approx(0.5f * center).epsilon(0.05));
    CHECK(left(out, 9500) == doctest::Approx(center));
}

TEST_CASE("stop с затуханием: тишина к концу, без — почти сразу; устаревший VoiceId безопасен") {
    AudioEngine engine(small_config());
    const ClipId dc = engine.add_clip("dc", make_constant(1.0f, 480'000));
    const VoiceId slow = engine.play(dc);
    render(engine, 1024);
    engine.stop(slow, 0.1f);
    const auto out = render(engine, 9600);
    CHECK(left(out, 100) > 0.4f);
    CHECK(left(out, 2400) < center);
    CHECK(left(out, 2400) > 0.0f);
    CHECK(peak(out, 5200) == 0.0f);
    CHECK_FALSE(engine.is_playing(slow));

    const VoiceId fast = engine.play(dc);
    render(engine, 1024);
    engine.stop(fast);
    CHECK(peak(render(engine, 1024), 256) == 0.0f);

    // Устаревшие и нулевые идентификаторы ничего не ломают.
    engine.stop(slow);
    engine.pause(slow, true);
    engine.set_voice_volume(slow, 3.0f);
    engine.set_voice_pitch(slow, 3.0f);
    engine.set_voice_pan(slow, 1.0f);
    engine.set_position(slow, {1, 2, 3});
    engine.stop(VoiceId{});
    CHECK_FALSE(engine.is_playing(VoiceId{}));
    CHECK(peak(render(engine, 512)) == 0.0f);
}

TEST_CASE("живое изменение громкости, высоты и панорамы") {
    AudioEngine engine(small_config());
    const VoiceId voice = engine.play(engine.add_clip("dc", make_constant(1.0f, 480'000)));
    render(engine, 1024);
    engine.set_voice_volume(voice, 0.5f);
    CHECK(left(render(engine, 1024), 1000) == doctest::Approx(0.5f * center));
    engine.set_voice_pan(voice, -1.0f);
    const auto out = render(engine, 1024);
    CHECK(left(out, 1000) == doctest::Approx(0.5f));
    CHECK(std::abs(right(out, 1000)) < 1e-4f);
}

TEST_CASE("3D: источник справа громче в правом канале и тише с расстоянием") {
    AudioEngine engine(small_config());
    const ClipId dc = engine.add_clip("dc", make_constant(1.0f, 480'000));
    Spatial s;
    s.enabled = true;
    s.position = {2, 0, 0};
    const VoiceId voice = engine.play(dc, {.spatial = s});
    auto out = render(engine, 2048);
    CHECK(right(out, 2000) > 0.2f);
    CHECK(left(out, 2000) < 0.01f);
    const float near_level = right(out, 2000);

    engine.set_position(voice, {20, 0, 0});
    out = render(engine, 2048);
    CHECK(right(out, 2000) < near_level * 0.2f);

    Listener l;
    l.position = {20, 0, 0}; // слушатель подошёл вплотную
    engine.set_listener(l);
    engine.set_position(voice, {20, 0, -1});
    out = render(engine, 2048);
    CHECK(left(out, 2000) == doctest::Approx(right(out, 2000)).epsilon(0.05)); // прямо по курсу → центр
}

TEST_CASE("3D-голос с сильным срезом высоких: шум теряет энергию") {
    const auto run = [](float occlusion) {
        AudioEngine engine(small_config());
        Spatial s;
        s.enabled = true;
        s.position = {0, 0, -1};
        s.occlusion = occlusion;
        engine.play(engine.add_clip("n", make_noise(1.0, 48'000, 3, 0.5f)), {.loop = true, .spatial = s});
        const auto out = render(engine, 24'000);
        return rms(out, 4800, 24'000);
    };
    CHECK(run(1.0f) < run(0.0f) * 0.5f);
}

TEST_CASE("Доплер меняет скорость чтения: удаляющийся источник играет дольше") {
    const auto frames_until_end = [](float velocity_z) {
        AudioEngine engine(small_config());
        Spatial s;
        s.enabled = true;
        s.position = {0, 0, -50};
        s.velocity = {0, 0, velocity_z};
        s.min_distance = 100.0f;
        const VoiceId voice = engine.play(engine.add_clip("dc", make_constant(1.0f, 24'000)), {.spatial = s});
        std::size_t frames = 0;
        while (engine.is_playing(voice) && frames < 200'000) { render(engine, 256); frames += 256; }
        return frames;
    };
    CHECK(frames_until_end(-100.0f) > frames_until_end(0.0f));
    CHECK(frames_until_end(100.0f) < frames_until_end(0.0f));
}

TEST_CASE("лимитер: сотня громких голосов не выходит за ±1") {
    AudioEngine engine(small_config());
    const ClipId noise = engine.add_clip("n", make_noise(1.0, 48'000, 1, 1.0f));
    for (int i = 0; i < 60; ++i) engine.play(noise, {.loop = true});
    const auto out = render(engine, 24'000);
    CHECK(peak(out) <= 1.0f);
    CHECK(peak(out) > 0.9f);
    CHECK(engine.stats().limiter_blocks > 0);
}

TEST_CASE("реверберация: хвост после конца звука есть только когда она включена") {
    const auto tail_energy = [](float wet) {
        AudioEngine engine(small_config());
        engine.set_reverb({.room_size = 0.8f, .damping = 0.3f, .wet = wet});
        engine.play(engine.add_clip("click", make_noise(0.02, 48'000, 2, 0.8f)), {.reverb_send = 1.0f});
        render(engine, 4800); // сам звук давно закончился
        return rms(render(engine, 24'000), 0, 24'000);
    };
    CHECK(tail_energy(0.0f) == 0.0f);
    CHECK(tail_energy(0.5f) > 1e-4f);
}

TEST_CASE("ducking: пока говорит голос, музыка тише, потом возвращается") {
    EngineConfig c = small_config();
    c.ducking = {DuckRule{.source = 3, .target = 2, .reduction_db = 12.0f, .threshold = 0.01f, .attack_seconds = 0.02f, .release_seconds = 0.1f}};
    AudioEngine engine(c);
    engine.play(engine.add_clip("m", make_constant(0.5f, 480'000)), {.bus = Buses::music});
    render(engine, 9600);
    const float before = engine.bus_meter(Buses::music).peak;

    const VoiceId talk = engine.play(engine.add_clip("v", make_constant(0.5f, 480'000)), {.bus = Buses::voice});
    render(engine, 24'000);
    const float during = engine.bus_meter(Buses::music).peak;
    CHECK(during < before * 0.4f); // −12 дБ ≈ 0.25

    engine.stop(talk);
    render(engine, 24'000);
    const float after = engine.bus_meter(Buses::music).peak;
    CHECK(after == doctest::Approx(before).epsilon(0.02));
}

TEST_CASE("срез высоких на шине: шум глохнет, снятие возвращает") {
    AudioEngine engine(small_config());
    engine.play(engine.add_clip("n", make_noise(1.0, 48'000, 4, 0.5f)), {.loop = true});
    const float open = rms(render(engine, 12'000), 2000, 12'000);
    engine.set_bus_lowpass(Buses::sfx, 500.0f);
    render(engine, 2048);
    const float muffled = rms(render(engine, 12'000), 2000, 12'000);
    CHECK(muffled < open * 0.3f);
    engine.set_bus_lowpass(Buses::sfx, 20'000.0f);
    render(engine, 2048);
    const float restored = rms(render(engine, 12'000), 2000, 12'000);
    CHECK(restored == doctest::Approx(open).epsilon(0.1));
}

TEST_CASE("измерители шин показывают уровень") {
    AudioEngine engine(small_config());
    CHECK(engine.bus_meter(Buses::master).peak == 0.0f);
    engine.play(engine.add_clip("dc", make_constant(0.5f, 48'000)), {.volume = 1.0f, .pan = 0.0f});
    render(engine, 2048);
    const BusMeter sfx = engine.bus_meter(Buses::sfx);
    const BusMeter master = engine.bus_meter(Buses::master);
    CHECK(sfx.peak == doctest::Approx(0.5f * center));
    CHECK(master.peak == doctest::Approx(sfx.peak));
    CHECK(sfx.rms == doctest::Approx(0.5f * center));
    CHECK(engine.bus_meter(Buses::music).peak == 0.0f);
}

TEST_CASE("переполнение очереди команд: лишние теряются и считаются, движок цел") {
    EngineConfig c = small_config();
    c.command_capacity = 64;
    AudioEngine engine(c);
    const ClipId clip = engine.add_clip("dc", make_constant(0.01f, 480'000));
    int refused = 0;
    for (int i = 0; i < 1000; ++i) refused += engine.play(clip) ? 0 : 1;
    CHECK(refused > 0);
    CHECK(engine.stats().dropped_commands == static_cast<std::uint64_t>(refused));
    render(engine, 1024);
    CHECK(engine.active_voices() == 64);
    engine.play(clip); // после опустошения очередь снова принимает
    render(engine, 256);
}

TEST_CASE("клипы: имена, замена, удаление не обрывает играющий голос") {
    AudioEngine engine(small_config());
    CHECK_FALSE(engine.add_clip("bad", AudioClip{}));
    const ClipId a = engine.add_clip("a", make_constant(0.5f, 48'000));
    CHECK(engine.find_clip("a") == a);
    CHECK_FALSE(engine.find_clip("missing"));
    CHECK(engine.clip(a)->frames() == 48'000);
    CHECK(engine.add_clip("a", make_constant(0.25f, 10)) == a); // замена под тем же id
    CHECK(engine.clip(a)->frames() == 10);

    const ClipId b = engine.add_clip("b", make_constant(0.5f, 48'000));
    const VoiceId voice = engine.play(b);
    render(engine, 512);
    engine.remove_clip(b);
    CHECK_FALSE(engine.find_clip("b"));
    CHECK_FALSE(engine.play(b));
    render(engine, 512);
    CHECK(engine.is_playing(voice));
    CHECK(engine.play(ClipId{}).value == 0);
    CHECK(engine.play(ClipId{999}).value == 0);
}

TEST_CASE("мусорные параметры воспроизведения зажимаются") {
    AudioEngine engine(small_config());
    const ClipId clip = engine.add_clip("dc", make_constant(0.1f, 48'000));
    engine.play(clip, {.volume = -5.0f, .pitch = -1.0f, .pan = 99.0f, .fade_in = -1.0f, .start_delay = -1.0f, .start_offset = 1e9f});
    engine.play(clip, {.volume = std::numeric_limits<float>::infinity() > 0 ? 1.0f : 0.0f, .pitch = 1000.0f});
    const auto out = render(engine, 4096);
    for (const float s : out) CHECK(std::isfinite(s));
    CHECK(peak(out) <= 1.0f);
}

TEST_CASE("управление из нескольких потоков во время рендера (прогон под TSan)") {
    AudioEngine engine(small_config());
    const ClipId clip = engine.add_clip("n", make_noise(0.05, 48'000, 8, 0.3f));
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 3; ++t) {
        workers.emplace_back([&, t] {
            std::uint32_t i = 0;
            while (!stop.load()) {
                const VoiceId v = engine.play(clip, {.pitch = 1.0f + 0.1f * static_cast<float>(t), .loop = (i % 3) == 0});
                engine.set_voice_volume(v, 0.5f, 0.01f);
                engine.set_bus_volume(Buses::sfx, 0.9f);
                if (i % 5 == 0) engine.stop(v, 0.01f);
                (void)engine.is_playing(v);
                (void)engine.bus_meter(Buses::master);
                ++i;
                std::this_thread::yield();
            }
        });
    }
    for (int i = 0; i < 400; ++i) {
        const auto out = render(engine, 512);
        REQUIRE(peak(out) <= 1.0f);
    }
    stop = true;
    for (auto& w : workers) w.join();
    engine.stop_all();
    render(engine, 4096);
    CHECK(engine.active_voices() == 0);
}

TEST_CASE("NullBackend и офлайн-рендер") {
    AudioEngine engine(small_config());
    NullBackend backend;
    CHECK(backend.pump(100).size() == 200); // без start — тишина нужной длины
    REQUIRE(backend.start(engine));
    engine.play(engine.add_clip("dc", make_constant(0.5f, 48'000)));
    CHECK(peak(backend.pump(4096)) > 0.0f);
    backend.stop();
    CHECK(std::string(backend.name()) == "null");

    AudioEngine offline(small_config());
    offline.play(offline.add_clip("s", make_sine(440.0f, 1.0)));
    const AudioClip clip = render_to_clip(offline, 0.5);
    CHECK(clip.channels == 2);
    CHECK(clip.frames() == 24'000);
    CHECK(offline.time_seconds() == doctest::Approx(0.5));
    const auto wav = decode_wav(encode_wav(clip, WavFormat::Float32));
    REQUIRE(wav);
    CHECK(wav->samples == clip.samples);
}

} // TEST_SUITE
