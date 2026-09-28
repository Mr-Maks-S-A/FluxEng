/**
 * @file test_sprite_batch.cpp
 * @brief Тесты SpriteBatch: вершины, отражения, повороты, сортировка, группировка по текстурам.
 */

#include <RendererSystem/Batch/SpriteBatch.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <numbers>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

using namespace RendererSystem;

namespace {

void check_near(glm::vec2 actual, glm::vec2 expected) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-5));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-5));
}

SpriteInstance textured(TextureHandle texture, std::int32_t layer = 0, float x = 0.0f) {
    return SpriteInstance{.position = {x, 0.0f}, .texture = texture, .layer = layer};
}

} // namespace

TEST_SUITE("Batch::SpriteBatch") {
    TEST_CASE("quad corners follow position, size and pivot") {
        SpriteVertex v[4];
        SpriteBatch::write_quad(SpriteInstance{.position = {100.0f, 50.0f}, .size = {20.0f, 10.0f}, .pivot = {0.5f, 0.5f}},
                                v);
        check_near(v[0].position, {90.0f, 45.0f});  // левый верх
        check_near(v[1].position, {110.0f, 45.0f}); // правый верх
        check_near(v[2].position, {110.0f, 55.0f}); // правый низ
        check_near(v[3].position, {90.0f, 55.0f});  // левый низ

        SpriteBatch::write_quad(SpriteInstance{.position = {100.0f, 50.0f}, .size = {20.0f, 10.0f}, .pivot = {0.0f, 0.0f}},
                                v);
        check_near(v[0].position, {100.0f, 50.0f});
        check_near(v[2].position, {120.0f, 60.0f});
    }

    TEST_CASE("UVs map image top-left to quad top-left; flips swap them") {
        const UvRect uv{{0.25f, 0.5f}, {0.5f, 1.0f}};
        SpriteVertex v[4];

        SpriteBatch::write_quad(SpriteInstance{.uv = uv}, v);
        check_near(v[0].uv, {0.25f, 0.5f});
        check_near(v[2].uv, {0.5f, 1.0f});

        SpriteBatch::write_quad(SpriteInstance{.uv = uv, .flip = SpriteFlip::X}, v);
        check_near(v[0].uv, {0.5f, 0.5f});
        check_near(v[1].uv, {0.25f, 0.5f});

        SpriteBatch::write_quad(SpriteInstance{.uv = uv, .flip = SpriteFlip::X | SpriteFlip::Y}, v);
        check_near(v[0].uv, {0.5f, 1.0f});
        check_near(v[2].uv, {0.25f, 0.5f});
    }

    TEST_CASE("rotation turns the quad around the pivot (clockwise with Y down)") {
        SpriteVertex v[4];
        SpriteBatch::write_quad(SpriteInstance{.position = {0.0f, 0.0f}, .size = {2.0f, 2.0f}, .pivot = {0.5f, 0.5f},
                                               .rotation = std::numbers::pi_v<float> / 2},
                                v);
        // Левый верх (-1,-1) после поворота на 90° оказывается справа сверху (1,-1).
        check_near(v[0].position, {1.0f, -1.0f});
        check_near(v[2].position, {-1.0f, 1.0f});
    }

    TEST_CASE("color is copied to every vertex") {
        SpriteVertex v[4];
        SpriteBatch::write_quad(SpriteInstance{.color = Colors::yellow}, v);
        for (const SpriteVertex& vertex : v) {
            CHECK(vertex.color == Colors::yellow);
        }
    }

    TEST_CASE("one texture, one layer: single draw command") {
        SpriteBatch batch;
        for (int i = 0; i < 100; ++i) {
            batch.submit(textured(TextureHandle{3}));
        }
        batch.build();
        CHECK(batch.quad_count() == 100);
        CHECK(batch.vertices().size() == 400);
        REQUIRE(batch.commands().size() == 1);
        CHECK(batch.commands()[0] == DrawCommand{TextureHandle{3}, 0, 100});
    }

    TEST_CASE("LayerThenTexture groups interleaved textures") {
        SpriteBatch batch(SortMode::LayerThenTexture);
        // A B A B A B → A A A | B B B
        for (int i = 0; i < 6; ++i) {
            batch.submit(textured(TextureHandle{i % 2 == 0 ? 1u : 2u}, 0, static_cast<float>(i)));
        }
        batch.build();
        REQUIRE(batch.commands().size() == 2);
        CHECK(batch.commands()[0] == DrawCommand{TextureHandle{1}, 0, 3});
        CHECK(batch.commands()[1] == DrawCommand{TextureHandle{2}, 3, 3});

        // Внутри группы порядок отправки сохраняется: x = 0, 2, 4.
        CHECK(batch.vertices()[0 * 4].position.x == doctest::Approx(-0.5f));
        CHECK(batch.vertices()[1 * 4].position.x == doctest::Approx(1.5f));
        CHECK(batch.vertices()[2 * 4].position.x == doctest::Approx(3.5f));
    }

    TEST_CASE("LayerThenSubmission keeps submission order inside a layer") {
        SpriteBatch batch(SortMode::LayerThenSubmission);
        for (int i = 0; i < 6; ++i) {
            batch.submit(textured(TextureHandle{i % 2 == 0 ? 1u : 2u}));
        }
        batch.build();
        CHECK(batch.commands().size() == 6);
    }

    TEST_CASE("layers are drawn from low to high, negative layers first") {
        SpriteBatch batch;
        batch.submit(textured(TextureHandle{1}, 5));
        batch.submit(textured(TextureHandle{1}, -3));
        batch.submit(textured(TextureHandle{2}, 0));
        batch.build();
        REQUIRE(batch.commands().size() == 3);
        CHECK(batch.commands()[0].texture == TextureHandle{1}); // слой -3
        CHECK(batch.commands()[1].texture == TextureHandle{2}); // слой 0
        CHECK(batch.commands()[2].texture == TextureHandle{1}); // слой 5
    }

    TEST_CASE("rect, line and outline helpers use the white texture") {
        SpriteBatch batch;
        batch.submit_rect({{10.0f, 20.0f}, {30.0f, 40.0f}}, Colors::red);
        batch.submit_line({0.0f, 0.0f}, {10.0f, 0.0f}, 2.0f, Colors::green);
        batch.submit_rect_outline({{0.0f, 0.0f}, {10.0f, 10.0f}}, 1.0f, Colors::blue);
        batch.build();

        CHECK(batch.quad_count() == 1 + 1 + 4);
        REQUIRE(batch.commands().size() == 1);
        CHECK(batch.commands()[0].texture == TextureHandle::white());

        // Прямоугольник: pivot в левом верхнем углу.
        check_near(batch.vertices()[0].position, {10.0f, 20.0f});
        check_near(batch.vertices()[2].position, {40.0f, 60.0f});
        // Линия от (0,0) до (10,0) толщиной 2: y от -1 до 1.
        check_near(batch.vertices()[4].position, {0.0f, -1.0f});
        check_near(batch.vertices()[6].position, {10.0f, 1.0f});
    }

    TEST_CASE("degenerate line and outline produce nothing") {
        SpriteBatch batch;
        batch.submit_line({5.0f, 5.0f}, {5.0f, 5.0f}, 2.0f, Colors::red);
        batch.submit_line({0.0f, 0.0f}, {1.0f, 0.0f}, 0.0f, Colors::red);
        batch.submit_rect_outline({{0.0f, 0.0f}, {0.0f, 10.0f}}, 1.0f, Colors::red);
        batch.build();
        CHECK(batch.quad_count() == 0);
        CHECK(batch.commands().empty());
    }

    TEST_CASE("clear resets the batch and keeps it reusable") {
        SpriteBatch batch;
        batch.reserve(16);
        batch.submit(textured(TextureHandle{1}));
        batch.build();
        batch.clear();
        CHECK(batch.sprites().empty());
        CHECK(batch.vertices().empty());
        CHECK(batch.commands().empty());

        batch.submit(textured(TextureHandle{2}));
        batch.build();
        REQUIRE(batch.commands().size() == 1);
        CHECK(batch.commands()[0].texture == TextureHandle{2});
    }

    TEST_CASE("large batches (radix sort path) match a reference stable sort") {
        std::mt19937 rng(42);
        std::uniform_int_distribution<std::int32_t> layer(-70000, 70000); // затрагивает несколько байт ключа
        std::uniform_int_distribution<std::uint32_t> texture(0, 300);

        for (const SortMode mode : {SortMode::LayerThenTexture, SortMode::LayerThenSubmission}) {
            SpriteBatch batch(mode);
            std::vector<SpriteInstance> sprites;
            for (int i = 0; i < 5000; ++i) {
                sprites.push_back(SpriteInstance{.position = {static_cast<float>(i), 0.0f},
                                                 .texture = TextureHandle{texture(rng)}, .layer = layer(rng)});
                batch.submit(sprites.back());
            }
            batch.build();

            // Эталон: стабильная сортировка индексов по (слой, текстура или 0).
            std::vector<std::size_t> expected(sprites.size());
            std::iota(expected.begin(), expected.end(), std::size_t{0});
            std::ranges::stable_sort(expected, [&](std::size_t a, std::size_t b) {
                const auto key = [&](std::size_t i) {
                    return std::pair{sprites[i].layer,
                                     mode == SortMode::LayerThenTexture ? sprites[i].texture.index : 0u};
                };
                return key(a) < key(b);
            });

            bool same_order = true;
            for (std::size_t quad = 0; quad < expected.size(); ++quad) {
                // x левого верхнего угла = номер спрайта - 0.5
                same_order = same_order &&
                             batch.vertices()[quad * 4].position.x == static_cast<float>(expected[quad]) - 0.5f;
            }
            CHECK(same_order);
        }
    }

    TEST_CASE("build of an empty batch") {
        SpriteBatch batch;
        batch.build();
        CHECK(batch.quad_count() == 0);
        CHECK(batch.commands().empty());
    }
}
