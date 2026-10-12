// Сколько стоит секунда звука. Бюджет аудиопотока: блок 512 кадров при 48 кГц — 10.7 мс; рендер должен занимать малую долю.
#include <AudioSystem/AudioSystem.hpp>

#include <benchmark/benchmark.h>

#include <memory>

using namespace AudioSystem;

namespace {

std::unique_ptr<AudioEngine> make_engine(int voices, bool spatial, bool reverb) {
    EngineConfig config;
    config.max_voices = 256;
    auto owner = std::make_unique<AudioEngine>(config);
    AudioEngine& engine = *owner;
    const ClipId clip = engine.add_clip("noise", make_noise(2.0, 44'100, 1, 0.1f));
    if (reverb) engine.set_reverb({.room_size = 0.8f, .damping = 0.4f, .wet = 0.3f});
    for (int i = 0; i < voices; ++i) {
        PlayParams p;
        p.loop = true;
        p.pitch = 0.9f + 0.002f * static_cast<float>(i % 100);
        p.reverb_send = reverb ? 0.3f : 0.0f;
        p.bus = BusId{static_cast<std::uint16_t>(1 + i % 5)};
        if (spatial) {
            p.spatial.enabled = true;
            p.spatial.position = {static_cast<float>(i % 17) - 8.0f, 0.0f, -static_cast<float>(i % 23) - 1.0f};
            p.spatial.velocity = {1.0f, 0.0f, 0.0f};
            p.spatial.air_absorption = 0.02f;
        }
        engine.play(clip, p);
    }
    std::vector<float> warm(512 * 2);
    engine.render(warm);
    return owner;
}

void render_voices(benchmark::State& state, bool spatial, bool reverb) {
    const auto owner = make_engine(static_cast<int>(state.range(0)), spatial, reverb);
    AudioEngine& engine = *owner;
    std::vector<float> buffer(512 * 2);
    for (auto _ : state) {
        engine.render(buffer);
        benchmark::DoNotOptimize(buffer.data());
    }
    state.SetItemsProcessed(state.iterations() * 512);
    state.counters["realtime_x"] = benchmark::Counter(static_cast<double>(state.iterations()) * 512.0 / 48'000.0, benchmark::Counter::kIsRate);
}

void BM_Render_2D(benchmark::State& s) { render_voices(s, false, false); }
void BM_Render_3D(benchmark::State& s) { render_voices(s, true, false); }
void BM_Render_3D_Reverb(benchmark::State& s) { render_voices(s, true, true); }

void BM_Reverb(benchmark::State& state) {
    Reverb reverb(48'000);
    reverb.set({.room_size = 0.8f, .damping = 0.4f, .wet = 0.3f});
    std::vector<float> in(512, 0.1f), out_l(512), out_r(512);
    for (auto _ : state) {
        reverb.process(in.data(), in.data(), out_l.data(), out_r.data(), 512);
        benchmark::DoNotOptimize(out_l.data());
    }
    state.SetItemsProcessed(state.iterations() * 512);
}

void BM_ComputeSpatial(benchmark::State& state) {
    Spatial s;
    s.enabled = true;
    s.position = {10, 2, -30};
    s.velocity = {3, 0, 1};
    s.air_absorption = 0.02f;
    s.cone_inner_degrees = 90;
    Listener l;
    for (auto _ : state) {
        s.position.x += 0.001f;
        benchmark::DoNotOptimize(compute_spatial(s, l));
    }
}

void BM_PlayCommand(benchmark::State& state) {
    AudioEngine engine;
    const ClipId clip = engine.add_clip("c", make_constant(0.1f, 1000));
    std::vector<float> buffer(512 * 2);
    for (auto _ : state) {
        for (int i = 0; i < 32; ++i) benchmark::DoNotOptimize(engine.play(clip));
        engine.render(buffer);
    }
    state.SetItemsProcessed(state.iterations() * 32);
}

void BM_DecodeWav(benchmark::State& state) {
    const auto wav = encode_wav(make_noise(5.0, 48'000, 1));
    for (auto _ : state) benchmark::DoNotOptimize(decode_wav(wav));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wav.size()));
}

} // namespace

BENCHMARK(BM_Render_2D)->Arg(16)->Arg(64)->Arg(256);
BENCHMARK(BM_Render_3D)->Arg(16)->Arg(64)->Arg(256);
BENCHMARK(BM_Render_3D_Reverb)->Arg(64);
BENCHMARK(BM_Reverb);
BENCHMARK(BM_ComputeSpatial);
BENCHMARK(BM_PlayCommand);
BENCHMARK(BM_DecodeWav);

BENCHMARK_MAIN();
