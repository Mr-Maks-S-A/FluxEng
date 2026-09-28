/**
 * @file test_animation.cpp
 * @brief Тесты анимаций: сетки спрайт-листов, библиотека клипов, продвижение состояний.
 */

#include <RendererSystem/Animation/Animation.hpp>
#include <RendererSystem/Core/Error.hpp>

#include <doctest/doctest.h>

#include <vector>

using namespace RendererSystem;

namespace {

/// Клип из `count` кадров по 0.1 с.
AnimationClip make_clip(std::string name, int count, bool looping) {
    return AnimationClip{
        .name = std::move(name),
        .frames = make_grid_frames({.columns = 4, .rows = 2}, 0, count, 0.1f),
        .looping = looping,
    };
}

} // namespace

TEST_SUITE("Animation::Grid") {
    TEST_CASE("frames are numbered left to right, top to bottom") {
        const auto frames = make_grid_frames({.columns = 4, .rows = 2}, 3, 2, 0.25f);
        REQUIRE(frames.size() == 2);
        // Кадр 3 — последний в верхней строке.
        CHECK(frames[0].uv.min == glm::vec2{0.75f, 0.0f});
        CHECK(frames[0].uv.max == glm::vec2{1.0f, 0.5f});
        // Кадр 4 — первый во второй строке.
        CHECK(frames[1].uv.min == glm::vec2{0.0f, 0.5f});
        CHECK(frames[1].duration == 0.25f);
    }

    TEST_CASE("invalid grids and ranges are rejected") {
        CHECK_THROWS_AS((void)make_grid_frames({.columns = 0, .rows = 1}, 0, 1, 0.1f), RendererError);
        CHECK_THROWS_AS((void)make_grid_frames({.columns = 2, .rows = 2}, 3, 2, 0.1f), RendererError);
        CHECK_THROWS_AS((void)make_grid_frames({.columns = 2, .rows = 2}, -1, 1, 0.1f), RendererError);
        CHECK_THROWS_AS((void)make_grid_frames({.columns = 2, .rows = 2}, 0, 0, 0.1f), RendererError);
    }
}

TEST_SUITE("Animation::Library") {
    TEST_CASE("add, find and access clips") {
        AnimationLibrary library;
        const ClipId walk = library.add(make_clip("goblin.walk", 4, true));
        const ClipId die = library.add(make_clip("goblin.die", 3, false));

        CHECK(library.size() == 2);
        CHECK(library.find("goblin.die") == die);
        CHECK_FALSE(library.find("goblin.fly").has_value());
        CHECK(library.clip(walk).frames.size() == 4);
        CHECK(library.duration(walk) == doctest::Approx(0.4f));
        CHECK(library.contains(walk));
        CHECK_FALSE(library.contains(ClipId{}));
        CHECK_THROWS_AS((void)library.clip(ClipId{7}), RendererError);
    }

    TEST_CASE("invalid clips are rejected") {
        AnimationLibrary library;
        library.add(make_clip("a", 1, true));
        CHECK_THROWS_AS(library.add(make_clip("a", 1, true)), RendererError);  // дубликат
        CHECK_THROWS_AS(library.add(make_clip("", 1, true)), RendererError);   // без имени
        CHECK_THROWS_AS(library.add(AnimationClip{.name = "empty", .frames = {}, .looping = true}), RendererError);
        CHECK_THROWS_AS(library.add(AnimationClip{.name = "zero", .frames = {AnimationFrame{.uv = {}, .duration = 0.0f}},
                                                  .looping = true}),
                        RendererError);
        CHECK(library.size() == 1);
    }
}

TEST_SUITE("Animation::Playback") {
    TEST_CASE("frames advance with time") {
        AnimationLibrary library;
        const ClipId walk = library.add(make_clip("walk", 4, true));
        std::vector<AnimationState> states{AnimationState::start(walk)};

        advance_animations(states, library, 0.05f);
        CHECK(states[0].frame == 0);
        advance_animations(states, library, 0.06f);
        CHECK(states[0].frame == 1);
        CHECK(states[0].time == doctest::Approx(0.01f));
        CHECK(current_uv(states[0], library) == library.clip(walk).frames[1].uv);
    }

    TEST_CASE("looping clip wraps, even with a huge dt") {
        AnimationLibrary library;
        const ClipId walk = library.add(make_clip("walk", 4, true));
        std::vector<AnimationState> states{AnimationState::start(walk)};

        advance_animations(states, library, 0.45f); // 4.5 кадра
        CHECK(states[0].frame == 0);
        CHECK(states[0].time == doctest::Approx(0.05f));

        advance_animations(states, library, 1'000'000.05f); // целые обороты отбрасываются сразу
        CHECK(states[0].frame < 4);
        CHECK_FALSE(is_finished(states[0], library));
    }

    TEST_CASE("non-looping clip stops on the last frame") {
        AnimationLibrary library;
        const ClipId die = library.add(make_clip("die", 3, false));
        std::vector<AnimationState> states{AnimationState::start(die)};

        advance_animations(states, library, 0.25f);
        CHECK(states[0].frame == 2);
        CHECK_FALSE(is_finished(states[0], library));

        advance_animations(states, library, 10.0f);
        CHECK(states[0].frame == 2);
        CHECK(is_finished(states[0], library));
    }

    TEST_CASE("speed scales time; zero speed pauses") {
        AnimationLibrary library;
        const ClipId walk = library.add(make_clip("walk", 4, true));
        std::vector<AnimationState> states{AnimationState::start(walk, 2.0f), AnimationState::start(walk, 0.0f)};

        advance_animations(states, library, 0.1f);
        CHECK(states[0].frame == 2);
        CHECK(states[1].frame == 0);
        CHECK(states[1].time == 0.0f);
    }

    TEST_CASE("many states with different clips update in one call") {
        AnimationLibrary library;
        const ClipId walk = library.add(make_clip("walk", 4, true));
        const ClipId idle = library.add(make_clip("idle", 2, true));

        std::vector<AnimationState> states;
        for (int i = 0; i < 1000; ++i) {
            states.push_back(AnimationState::start(i % 2 == 0 ? walk : idle));
        }
        advance_animations(states, library, 0.15f);
        CHECK(states[0].frame == 1);
        CHECK(states[1].frame == 1);
    }

    TEST_CASE("invalid clip ids are skipped") {
        AnimationLibrary library;
        std::vector<AnimationState> states{AnimationState{}};
        advance_animations(states, library, 1.0f);
        CHECK(states[0].frame == 0);
        CHECK(current_uv(states[0], library) == UvRect{});
        CHECK_FALSE(is_finished(states[0], library));
    }
}
