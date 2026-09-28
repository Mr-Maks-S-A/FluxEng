/**
 * @file test_camera.cpp
 * @brief Тесты Camera2D: матрицы и перевод координат.
 */

#include <RendererSystem/Scene/Camera2D.hpp>

#include <doctest/doctest.h>

#include <glm/vec4.hpp>

#include <numbers>

using namespace RendererSystem;

namespace {

/// Мир → NDC через view_projection.
glm::vec2 to_ndc(const Camera2D& camera, glm::vec2 world) {
    const glm::vec4 clip = camera.view_projection() * glm::vec4(world, 0.0f, 1.0f);
    return {clip.x / clip.w, clip.y / clip.w};
}

void check_near(glm::vec2 actual, glm::vec2 expected) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-4));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-4));
}

} // namespace

TEST_SUITE("Scene::Camera2D") {
    TEST_CASE("camera position is the screen center") {
        const Camera2D camera{.position = {100.0f, 50.0f}, .zoom = 1.0f, .rotation = 0.0f, .viewport = {800.0f, 600.0f}};
        check_near(to_ndc(camera, {100.0f, 50.0f}), {0.0f, 0.0f});
        check_near(camera.world_to_screen({100.0f, 50.0f}), {400.0f, 300.0f});
    }

    TEST_CASE("Y axis points down: larger world Y is lower on screen") {
        const Camera2D camera{.position = {0.0f, 0.0f}, .zoom = 1.0f, .rotation = 0.0f, .viewport = {800.0f, 600.0f}};
        // Верх экрана (NDC y = +1) — это мир y = -300.
        check_near(to_ndc(camera, {0.0f, -300.0f}), {0.0f, 1.0f});
        check_near(to_ndc(camera, {400.0f, 300.0f}), {1.0f, -1.0f});
        check_near(camera.world_to_screen({-400.0f, -300.0f}), {0.0f, 0.0f});
    }

    TEST_CASE("zoom scales world units") {
        const Camera2D camera{.position = {0.0f, 0.0f}, .zoom = 2.0f, .rotation = 0.0f, .viewport = {800.0f, 600.0f}};
        check_near(to_ndc(camera, {200.0f, 0.0f}), {1.0f, 0.0f});
        check_near(camera.world_to_screen({10.0f, 10.0f}), {420.0f, 320.0f});
    }

    TEST_CASE("screen_to_world inverts world_to_screen, including rotation") {
        const Camera2D camera{.position = {12.0f, -7.0f}, .zoom = 1.5f, .rotation = 0.6f, .viewport = {640.0f, 360.0f}};
        for (const glm::vec2 world : {glm::vec2{0.0f, 0.0f}, glm::vec2{100.0f, -40.0f}, glm::vec2{-3.5f, 8.25f}}) {
            check_near(camera.screen_to_world(camera.world_to_screen(world)), world);
        }
    }

    TEST_CASE("matrix and screen mapping agree under rotation") {
        const Camera2D camera{.position = {5.0f, 5.0f}, .zoom = 1.0f, .rotation = std::numbers::pi_v<float> / 2,
                              .viewport = {200.0f, 200.0f}};
        const glm::vec2 world{30.0f, 5.0f};
        const glm::vec2 ndc = to_ndc(camera, world);
        const glm::vec2 screen = camera.world_to_screen(world);
        // NDC (-1..1, y вверх) → пиксели (0..200, y вниз).
        check_near({(ndc.x + 1.0f) * 100.0f, (1.0f - ndc.y) * 100.0f}, screen);
    }
}
