/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки RendererSystem (google-benchmark).
 *
 * CPU (работают везде):
 * - SpriteBatch::build — генерация вершин и сортировка: одна текстура, много текстур, повороты;
 * - advance_animations — обновление тысяч анимированных сущностей.
 *
 * GPU (на каждом доступном бэкенде RHI — OpenGL и Vulkan; недоступный пропускается):
 * - полный кадр Renderer2D в RenderTarget 1080p с ожиданием GPU — цена загрузки вершин и draw call'ов;
 * - кадр Renderer3D: сотни лит-объектов (uniform-блок на вызов) — цена вызова рисования.
 *
 * Для осмысленных цифр собирайте в Release.
 */

#include "../tests/GlTestContext.hpp"

#include <RendererSystem/RendererSystem.hpp>

#include <benchmark/benchmark.h>

#include <glm/ext/matrix_transform.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace RendererSystem;

namespace {

constexpr std::int64_t min_sprites = 1 << 10;
constexpr std::int64_t max_sprites = 1 << 17;

/// Случайные спрайты по экрану 1920×1080 с `textures` разными текстурами.
std::vector<SpriteInstance> make_sprites(std::size_t count, std::uint32_t textures, bool rotated) {
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> x(0.0f, 1920.0f);
    std::uniform_real_distribution<float> y(0.0f, 1080.0f);
    std::uniform_real_distribution<float> angle(0.0f, 6.28f);
    std::uniform_int_distribution<std::uint32_t> texture(1, textures);

    std::vector<SpriteInstance> sprites(count);
    for (SpriteInstance& sprite : sprites) {
        sprite.position = {x(rng), y(rng)};
        sprite.size = {16.0f, 16.0f};
        sprite.rotation = rotated ? angle(rng) : 0.0f;
        sprite.texture = TextureHandle{texture(rng)};
    }
    return sprites;
}

// =============================================================================
// CPU
// =============================================================================

void BM_SpriteBatch_Build(benchmark::State& state, std::uint32_t textures, bool rotated, SortMode mode) {
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto sprites = make_sprites(count, textures, rotated);
    SpriteBatch batch(mode);
    batch.reserve(count);

    for (auto _ : state) {
        batch.clear();
        for (const SpriteInstance& sprite : sprites) {
            batch.submit(sprite);
        }
        batch.build();
        benchmark::DoNotOptimize(batch.vertices().data());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["draw_calls"] = static_cast<double>(batch.commands().size());
}
BENCHMARK_CAPTURE(BM_SpriteBatch_Build, one_texture, 1u, false, SortMode::LayerThenTexture)
    ->Range(min_sprites, max_sprites);
BENCHMARK_CAPTURE(BM_SpriteBatch_Build, one_texture_rotated, 1u, true, SortMode::LayerThenTexture)
    ->Range(min_sprites, max_sprites);
BENCHMARK_CAPTURE(BM_SpriteBatch_Build, 16_textures_sorted, 16u, false, SortMode::LayerThenTexture)
    ->Range(min_sprites, max_sprites);
BENCHMARK_CAPTURE(BM_SpriteBatch_Build, 16_textures_submission, 16u, false, SortMode::LayerThenSubmission)
    ->Range(min_sprites, max_sprites);

void BM_WriteQuad(benchmark::State& state) {
    const SpriteInstance sprite{.position = {10.0f, 20.0f}, .size = {16.0f, 16.0f}, .rotation = 0.3f};
    SpriteVertex vertices[4];
    for (auto _ : state) {
        SpriteBatch::write_quad(sprite, vertices);
        benchmark::DoNotOptimize(vertices);
    }
}
BENCHMARK(BM_WriteQuad);

void BM_AdvanceAnimations(benchmark::State& state) {
    AnimationLibrary library;
    std::vector<ClipId> clips;
    for (int i = 0; i < 8; ++i) {
        clips.push_back(library.add(AnimationClip{
            .name = "clip_" + std::to_string(i),
            .frames = make_grid_frames({.columns = 8, .rows = 8}, i * 8, 8, 0.08f + 0.01f * static_cast<float>(i)),
            .looping = i != 7,
        }));
    }
    std::vector<AnimationState> states;
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        states.push_back(AnimationState::start(clips[static_cast<std::size_t>(i) % clips.size()]));
    }

    for (auto _ : state) {
        advance_animations(states, library, 1.0f / 60.0f);
        benchmark::DoNotOptimize(states.data());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_AdvanceAnimations)->Range(min_sprites, max_sprites);

// =============================================================================
// GPU
// =============================================================================

/// Устройство бэкенда (одно на процесс, не уничтожается — как GlTestContext) или nullptr.
RHI::Device* device_for(Backend backend) {
    static RHI::Device* gl = [] () -> RHI::Device* {
        if (!RendererTests::GlTestContext::instance().available()) return nullptr;
        auto device = RHI::Device::create({.backend = Backend::OpenGL});
        return device ? device->release() : nullptr;
    }();
    static RHI::Device* vk = [] () -> RHI::Device* {
        auto device = RHI::Device::create({.backend = Backend::Vulkan});
        return device ? device->release() : nullptr;
    }();
    if (backend == Backend::OpenGL && gl != nullptr) RendererTests::GlTestContext::instance().make_current();
    return backend == Backend::OpenGL ? gl : vk;
}

void BM_Renderer2D_Frame(benchmark::State& state, Backend backend, std::uint32_t textures) {
    RHI::Device* device = device_for(backend);
    if (device == nullptr) {
        state.SkipWithError("backend is not available");
        return;
    }
    auto renderer = Renderer2D::create(*device);
    auto target = RenderTarget::create(*device, 1920, 1080);
    if (!renderer || !target) {
        state.SkipWithError("cannot create renderer or render target");
        return;
    }
    for (std::uint32_t i = 0; i < textures; ++i) {
        (void)renderer->create_texture(Image::checkerboard(16, 16, 4, Colors::white, Color::from_rgba(0x808080FF)));
    }

    const auto sprites = make_sprites(static_cast<std::size_t>(state.range(0)), textures, false);
    const Camera2D camera{.position = {960.0f, 540.0f}, .viewport = {1920.0f, 1080.0f}};

    for (auto _ : state) {
        device->begin_frame(1920, 1080);
        target->bind();
        renderer->clear(Colors::black);
        renderer->begin(camera);
        for (const SpriteInstance& sprite : sprites) {
            renderer->draw(sprite);
        }
        renderer->end();
        device->end_frame();
        device->wait_idle(); // включаем в замер работу GPU, а не только постановку команд
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["draw_calls"] = renderer->last_stats().draw_calls;
}
BENCHMARK_CAPTURE(BM_Renderer2D_Frame, opengl_one_texture, Backend::OpenGL, 1u)->Range(min_sprites, max_sprites)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Renderer2D_Frame, opengl_16_textures, Backend::OpenGL, 16u)->Range(min_sprites, max_sprites)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Renderer2D_Frame, vulkan_one_texture, Backend::Vulkan, 1u)->Range(min_sprites, max_sprites)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Renderer2D_Frame, vulkan_16_textures, Backend::Vulkan, 16u)->Range(min_sprites, max_sprites)->Unit(benchmark::kMicrosecond);

void BM_Renderer3D_Objects(benchmark::State& state, Backend backend) {
    RHI::Device* device = device_for(backend);
    if (device == nullptr) {
        state.SkipWithError("backend is not available");
        return;
    }
    auto renderer = Renderer3D::create(*device);
    auto target = RenderTarget::create(*device, 1280, 720, {.depth = true});
    if (!renderer || !target) {
        state.SkipWithError("cannot create renderer or render target");
        return;
    }
    const auto count = static_cast<int>(state.range(0));
    const Camera3D camera{.position = {0.0f, 20.0f, 30.0f}, .viewport = {1280.0f, 720.0f}};
    for (auto _ : state) {
        device->begin_frame(1280, 720);
        target->bind();
        renderer->clear(Colors::black);
        renderer->begin(camera);
        for (int i = 0; i < count; ++i) {
            const glm::vec3 at{static_cast<float>(i % 32) - 16.0f, 0.0f, static_cast<float>(i / 32) - 16.0f};
            renderer->draw_shape(i % 2 ? Renderer3D::Shape::Cube : Renderer3D::Shape::Sphere,
                                 glm::scale(glm::translate(glm::mat4{1.0f}, at), glm::vec3{0.8f}), {.color = Colors::white});
        }
        renderer->end();
        device->end_frame();
        device->wait_idle();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK_CAPTURE(BM_Renderer3D_Objects, opengl, Backend::OpenGL)->Range(64, 1024)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Renderer3D_Objects, vulkan, Backend::Vulkan)->Range(64, 1024)->Unit(benchmark::kMicrosecond);

} // namespace

BENCHMARK_MAIN();
