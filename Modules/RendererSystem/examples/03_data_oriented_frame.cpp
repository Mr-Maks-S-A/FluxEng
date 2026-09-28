/**
 * @example 03_data_oriented_frame.cpp
 * CPU-часть кадра без видеокарты: данные сущностей в плоских массивах (как в ECS),
 * одна функция обновляет все анимации, батч собирается из массивов и показывает,
 * сколько draw call'ов получится. Работает на сервере и в CI.
 */

#include <RendererSystem/RendererSystem.hpp>

#include <cstdint>
#include <print>
#include <vector>

using namespace RendererSystem;

int main() {
    // --- ресурсы: дескрипторы текстур выдаёт Renderer2D; здесь просто договоримся о номерах
    constexpr TextureHandle terrain_atlas{1};
    constexpr TextureHandle creatures_atlas{2};
    constexpr TextureHandle items_atlas{3};

    AnimationLibrary animations;
    const ClipId walk = animations.add(AnimationClip{
        .name = "dwarf.walk", .frames = make_grid_frames({.columns = 8, .rows = 8}, 0, 6, 0.1f), .looping = true});
    const ClipId mine = animations.add(AnimationClip{
        .name = "dwarf.mine", .frames = make_grid_frames({.columns = 8, .rows = 8}, 8, 4, 0.12f), .looping = true});

    // --- «компоненты»: плотные массивы одинаковой длины
    constexpr std::size_t dwarves = 2000;
    std::vector<glm::vec2> positions(dwarves);
    std::vector<AnimationState> anim(dwarves);
    for (std::size_t i = 0; i < dwarves; ++i) {
        positions[i] = {static_cast<float>(i % 50) * 16.0f, static_cast<float>(i / 50) * 16.0f};
        anim[i] = AnimationState::start(i % 3 == 0 ? mine : walk);
    }

    // --- система анимаций: один проход по массиву
    advance_animations(anim, animations, 1.0f / 60.0f);

    // --- система отрисовки: массивы → батч
    SpriteBatch batch(SortMode::LayerThenTexture);
    batch.reserve(64 * 64 + dwarves * 2);

    for (int y = 0; y < 64; ++y) { // карта 64×64 тайлов
        for (int x = 0; x < 64; ++x) {
            batch.submit(SpriteInstance{.position = {static_cast<float>(x) * 16.0f, static_cast<float>(y) * 16.0f}, .size = {16.0f, 16.0f},
                                        .pivot = {0.0f, 0.0f}, .texture = terrain_atlas, .layer = 0});
        }
    }
    for (std::size_t i = 0; i < dwarves; ++i) {
        batch.submit(SpriteInstance{.position = positions[i], .size = {16.0f, 16.0f},
                                    .uv = current_uv(anim[i], animations), .texture = creatures_atlas, .layer = 1});
        // Предмет в руках — вперемешку с существами, но в слое выше.
        batch.submit(SpriteInstance{.position = positions[i] + glm::vec2{4.0f, 0.0f}, .size = {8.0f, 8.0f},
                                    .texture = items_atlas, .layer = 2});
    }
    batch.build();

    std::println("sprites submitted: {}", batch.sprites().size());
    std::println("vertices built:    {} ({} KiB)", batch.vertices().size(),
                 batch.vertices().size_bytes() / 1024);
    std::println("draw commands:     {}", batch.commands().size());
    for (const DrawCommand& command : batch.commands()) {
        std::println("  texture {} : quads [{}, {})", command.texture.index, command.first_quad,
                     command.first_quad + command.quad_count);
    }

    // Тот же набор в порядке отправки внутри слоя (как для UI).
    batch.set_sort_mode(SortMode::LayerThenSubmission);
    batch.build();
    std::println("with LayerThenSubmission: {} draw commands", batch.commands().size());
}
