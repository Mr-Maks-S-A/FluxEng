#include <AudioSystem/AudioSystem.hpp>

#include "TestSupport.hpp"

#include <doctest/doctest.h>

#include <set>

using namespace AudioSystem;
using namespace test;

namespace {
void add_steps(AudioEngine& engine) {
    engine.add_clip("step_1", make_constant(0.1f, 4800));
    engine.add_clip("step_2", make_constant(0.2f, 4800));
    engine.add_clip("step_3", make_constant(0.3f, 4800));
}
} // namespace

TEST_SUITE("AudioSystem.SoundEvents") {

TEST_CASE("текст описаний разбирается, а to_text → load_text возвращает то же самое") {
    AudioEngine engine(small_config());
    SoundEvents events(engine);
    const auto count = events.load_text(R"(
        # шаги
        [footstep]
        clips = step_1 step_2   step_3
        bus = sfx
        volume = 0.8
        volume_var = 0.1     # разброс
        pitch = 1.1
        pitch_var = 1.5
        spatial = true
        min_distance = 2
        max_distance = 30
        rolloff = 1.5
        model = exponential
        priority = 77
        max_instances = 4
        cooldown = 0.08
        reverb_send = 0.25
        fade_in = 0.01
        avoid_repeat = false

        [ui_click]
        clips = step_1
        bus = ui
    )");
    REQUIRE(count);
    CHECK(*count == 2);
    const SoundEventDef* d = events.find("footstep");
    REQUIRE(d);
    CHECK(d->clips == std::vector<std::string>{"step_1", "step_2", "step_3"});
    CHECK(d->volume == doctest::Approx(0.8f));
    CHECK(d->spatial);
    CHECK(d->model == AttenuationModel::Exponential);
    CHECK(d->priority == 77);
    CHECK(d->max_instances == 4);
    CHECK_FALSE(d->avoid_repeat);

    AudioEngine other(small_config());
    SoundEvents copy(other);
    REQUIRE(copy.load_text(events.to_text()));
    CHECK(copy.to_text() == events.to_text());
    CHECK(copy.size() == 2);
}

TEST_CASE("ошибки текста называют строку и ничего не добавляют") {
    AudioEngine engine(small_config());
    SoundEvents events(engine);
    struct Case { const char* text; const char* needle; };
    const Case cases[] = {
        {"[a]\nclips = x\nvolum = 1\n", "line 3: unknown key 'volum'"},
        {"clips = x\n", "line 1: key before any [event] header"},
        {"[a\nclips = x\n", "line 1: unterminated section header"},
        {"[]\nclips = x\n", "line 1: empty event name"},
        {"[a]\nclips = x\nvolume = loud\n", "line 3: bad number value 'loud'"},
        {"[a]\nclips = x\nloop = maybe\n", "line 3: bad bool"},
        {"[a]\nclips = x\nmodel = cubic\n", "line 3: bad model"},
        {"[a]\nclips = x\npriority = 999\n", "line 3: bad priority"},
        {"[a]\nclips = x\nvolume = -1\n", "line 3"},
        {"[a]\nclips = x\nvolume = nan\n", "line 3"},
        {"[a]\nclips = x\njust text\n", "line 3: expected 'key = value'"},
        {"[a]\nbus = sfx\n", "event 'a': no clips"},
        {"[ok]\nclips = x\n[bad]\nvolume = 1\n", "event 'bad': no clips"},
    };
    for (const Case& c : cases) {
        const auto result = events.load_text(c.text);
        REQUIRE_MESSAGE(!result, c.text);
        CHECK_MESSAGE(result.error().find(c.needle) != std::string::npos, c.text, " -> ", result.error());
    }
    CHECK(events.size() == 0); // разбор атомарен: «ok» из последнего случая не просочился
}

TEST_CASE("запуск события: клипы не повторяются подряд, разброс громкости в пределах") {
    AudioEngine engine(small_config());
    add_steps(engine);
    SoundEvents events(engine, 123);
    SoundEventDef d;
    d.name = "step";
    d.clips = {"step_1", "step_2", "step_3"};
    d.volume = 1.0f;
    REQUIRE(events.add(d));

    std::set<float> seen;
    float previous = -1.0f;
    for (int i = 0; i < 30; ++i) {
        const VoiceId v = events.post("step");
        REQUIRE(v);
        const auto out = render(engine, 512);
        const float level = left(out, 511) / 0.70710678f;
        CHECK(std::abs(level - previous) > 0.05f); // не тот же клип, что в прошлый раз
        previous = level;
        seen.insert(std::round(level * 10.0f));
        engine.stop(v);
        render(engine, 512);
    }
    CHECK(seen.size() == 3); // все три использованы
}

TEST_CASE("воспроизводимость: то же зерно — та же последовательность, другое — другая") {
    const auto sequence = [](std::uint64_t seed) {
        AudioEngine engine(small_config());
        add_steps(engine);
        SoundEvents events(engine, seed);
        SoundEventDef d;
        d.name = "step";
        d.clips = {"step_1", "step_2", "step_3"};
        d.volume_var = 0.3f;
        d.pitch_var = 2.0f;
        events.add(d);
        std::vector<float> trace;
        for (int i = 0; i < 12; ++i) {
            events.post("step");
            const auto out = render(engine, 512);
            trace.push_back(left(out, 511));
            engine.stop_all();
            render(engine, 512);
        }
        return trace;
    };
    CHECK(sequence(5) == sequence(5));
    CHECK(sequence(5) != sequence(6));
}

TEST_CASE("остывание считается по часам звука, а не по стене") {
    AudioEngine engine(small_config());
    add_steps(engine);
    SoundEvents events(engine);
    SoundEventDef d;
    d.name = "hit";
    d.clips = {"step_1"};
    d.cooldown = 0.5f;
    events.add(d);
    CHECK(events.post("hit"));
    CHECK_FALSE(events.post("hit")); // сразу — нельзя
    render(engine, 12'000);          // 0.25 с звука
    CHECK_FALSE(events.post("hit"));
    render(engine, 13'000);          // > 0.5 с
    CHECK(events.post("hit"));
}

TEST_CASE("лимит экземпляров: пока играют — новые не запускаются, закончились — снова можно") {
    AudioEngine engine(small_config());
    engine.add_clip("long", make_constant(0.1f, 12'000));
    SoundEvents events(engine);
    SoundEventDef d;
    d.name = "engine_hum";
    d.clips = {"long"};
    d.max_instances = 2;
    events.add(d);
    CHECK(events.post("engine_hum"));
    CHECK(events.post("engine_hum"));
    CHECK_FALSE(events.post("engine_hum"));
    render(engine, 13'000);
    CHECK(events.post("engine_hum"));
}

TEST_CASE("3D-событие: позиция уходит в голос; не-3D событие игнорирует позицию") {
    AudioEngine engine(small_config());
    engine.add_clip("dc", make_constant(1.0f, 48'000));
    SoundEvents events(engine);
    SoundEventDef spatial;
    spatial.name = "boom";
    spatial.clips = {"dc"};
    spatial.spatial = true;
    SoundEventDef flat = spatial;
    flat.name = "ui";
    flat.spatial = false;
    events.add(spatial);
    events.add(flat);

    events.post("boom", {10, 0, 0});
    auto out = render(engine, 2048);
    CHECK(right(out, 2000) > 3.0f * left(out, 2000));
    engine.stop_all();
    render(engine, 512);

    events.post("ui", {10, 0, 0});
    out = render(engine, 2048);
    CHECK(left(out, 2000) == doctest::Approx(right(out, 2000)));
}

TEST_CASE("неизвестное событие, нет клипа, неизвестная шина — отказ или запасной вариант, без падения") {
    AudioEngine engine(small_config());
    engine.add_clip("dc", make_constant(0.5f, 4800));
    SoundEvents events(engine);
    CHECK_FALSE(events.post("nope"));
    CHECK_FALSE(events.add(SoundEventDef{}));
    SoundEventDef no_clips;
    no_clips.name = "x";
    CHECK_FALSE(events.add(no_clips));

    SoundEventDef missing_clip;
    missing_clip.name = "ghost";
    missing_clip.clips = {"does_not_exist"};
    REQUIRE(events.add(missing_clip));
    CHECK_FALSE(events.post("ghost"));

    SoundEventDef odd_bus;
    odd_bus.name = "odd";
    odd_bus.clips = {"dc"};
    odd_bus.bus = "no_such_bus";
    REQUIRE(events.add(odd_bus));
    CHECK(events.post("odd")); // запасная шина sfx
    CHECK(peak(render(engine, 2048)) > 0.0f);
}

TEST_CASE("stop(name) останавливает все экземпляры") {
    AudioEngine engine(small_config());
    engine.add_clip("dc", make_constant(0.5f, 480'000));
    SoundEvents events(engine);
    SoundEventDef d;
    d.name = "loop";
    d.clips = {"dc"};
    d.loop = true;
    events.add(d);
    const VoiceId a = events.post("loop");
    const VoiceId b = events.post("loop");
    render(engine, 1024);
    CHECK(engine.is_playing(a));
    events.stop("loop");
    render(engine, 1024);
    CHECK_FALSE(engine.is_playing(a));
    CHECK_FALSE(engine.is_playing(b));
}

} // TEST_SUITE
