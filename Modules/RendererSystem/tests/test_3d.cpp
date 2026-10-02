/**
 * @file test_3d.cpp
 * @brief CPU-часть 3D: Camera3D, лучи и коробки, пирамида видимости, генераторы сеток, процедурные изображения.
 */

#include <RendererSystem/RendererSystem.hpp>

#include <doctest/doctest.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <numbers>

using namespace RendererSystem;

namespace {

Camera3D camera() {
    return Camera3D{.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}, .viewport = {800.0f, 600.0f}};
}

/// Все треугольники смотрят наружу: нормаль треугольника направлена от центра сетки.
bool faces_outward(const MeshData& mesh) {
    const glm::vec3 center = mesh.bounds().center();
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const glm::vec3 a = mesh.vertices[mesh.indices[i]].position;
        const glm::vec3 b = mesh.vertices[mesh.indices[i + 1]].position;
        const glm::vec3 c = mesh.vertices[mesh.indices[i + 2]].position;
        const glm::vec3 normal = glm::cross(b - a, c - a);
        if (glm::length(normal) < 1e-9f) continue; // вырожденные (полюса сферы)
        if (glm::dot(normal, (a + b + c) / 3.0f - center) < -1e-6f) return false;
    }
    return true;
}

} // namespace

TEST_SUITE("Camera3D") {
    TEST_CASE("the target projects to the centre of the screen and the centre ray points at it") {
        const Camera3D cam = camera();
        const ScreenPoint p = cam.world_to_screen({0.0f, 0.0f, 0.0f});
        CHECK(p.visible);
        CHECK(p.position.x == doctest::Approx(400.0f));
        CHECK(p.position.y == doctest::Approx(300.0f));
        const Ray ray = cam.screen_to_ray({400.0f, 300.0f});
        CHECK(ray.direction.z == doctest::Approx(-1.0f));
        CHECK(glm::length(ray.direction) == doctest::Approx(1.0f));
    }

    TEST_CASE("screen Y goes down, world Y goes up") {
        const Camera3D cam = camera();
        CHECK(cam.world_to_screen({0.0f, 1.0f, 0.0f}).position.y < 300.0f);
        CHECK(cam.world_to_screen({1.0f, 0.0f, 0.0f}).position.x > 400.0f);
    }

    TEST_CASE("screen_to_ray and world_to_screen are inverse") {
        const Camera3D cam = Camera3D::orbit({1.0f, 0.0f, -2.0f}, 0.7f, 0.5f, 12.0f, {1280.0f, 720.0f});
        for (const glm::vec3 point : {glm::vec3{1.0f, 0.0f, -2.0f}, glm::vec3{3.0f, 1.0f, 0.0f}, glm::vec3{-2.0f, -1.0f, -4.0f}}) {
            const ScreenPoint s = cam.world_to_screen(point);
            REQUIRE(s.visible);
            const Ray ray = cam.screen_to_ray(s.position);
            // Точка лежит на луче: расстояние от неё до луча ~0.
            const glm::vec3 to_point = point - ray.origin;
            const float along = glm::dot(to_point, ray.direction);
            CHECK(glm::length(to_point - ray.direction * along) < 1e-3f);
        }
    }

    TEST_CASE("points behind the camera are not visible") {
        CHECK_FALSE(camera().world_to_screen({0.0f, 0.0f, 20.0f}).visible);
    }

    TEST_CASE("orbit keeps the distance and looks at the target") {
        const Camera3D cam = Camera3D::orbit({0.0f, 0.0f, 0.0f}, 1.2f, 0.4f, 7.0f, {100.0f, 100.0f});
        CHECK(glm::length(cam.position) == doctest::Approx(7.0f));
        CHECK(glm::dot(cam.forward(), glm::normalize(-cam.position)) == doctest::Approx(1.0f));
        CHECK(glm::dot(cam.right(), cam.forward()) == doctest::Approx(0.0f).epsilon(1e-5));
    }
}

TEST_SUITE("Geometry3D") {
    TEST_CASE("ray hits a plane in front and misses one behind") {
        const Ray ray{{0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
        const auto t = intersect(ray, Plane::from_point_normal({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
        REQUIRE(t.has_value());
        CHECK(*t == doctest::Approx(4.0f));
        CHECK_FALSE(intersect(ray, Plane::from_point_normal({0.0f, 9.0f, 0.0f}, {0.0f, 1.0f, 0.0f})).has_value());
        CHECK_FALSE(intersect(Ray{{0, 5, 0}, {1, 0, 0}}, Plane{}).has_value()); // параллелен
    }

    TEST_CASE("ray against a box: entry distance, inside origin, miss") {
        const Aabb box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        const auto hit = intersect(Ray{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(4.0f));
        CHECK(*intersect(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, box) == doctest::Approx(0.0f));
        CHECK_FALSE(intersect(Ray{{-5.0f, 3.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box).has_value());
        CHECK_FALSE(intersect(Ray{{-5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, box).has_value()); // коробка позади
    }

    TEST_CASE("ray in local space of a transformed object (picking a rotated, scaled card)") {
        const glm::mat4 model = glm::scale(glm::rotate(glm::translate(glm::mat4{1.0f}, {3.0f, 0.0f, 0.0f}), std::numbers::pi_v<float> * 0.5f,
                                                       glm::vec3{1.0f, 0.0f, 0.0f}),
                                           glm::vec3{2.0f});
        const Aabb local{{-0.5f, -0.5f, -0.05f}, {0.5f, 0.5f, 0.05f}};
        const Ray down{{3.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
        const auto t = intersect(down.transformed(glm::inverse(model)), local);
        REQUIRE(t.has_value());
        CHECK(down.at(*t).y == doctest::Approx(0.1f).epsilon(1e-4)); // толщина 0.05 × масштаб 2
    }

    TEST_CASE("Aabb: transformed, expand, empty") {
        Aabb box = Aabb::empty();
        CHECK_FALSE(box.valid());
        box.expand({1.0f, 2.0f, 3.0f});
        box.expand({-1.0f, 0.0f, 0.0f});
        CHECK(box.valid());
        CHECK(box.size() == glm::vec3{2.0f, 2.0f, 3.0f});
        const Aabb moved = box.transformed(glm::translate(glm::mat4{1.0f}, {10.0f, 0.0f, 0.0f}));
        CHECK(moved.min.x == doctest::Approx(9.0f));
        CHECK(moved.intersects(Aabb::from_center({10.0f, 1.0f, 1.0f}, glm::vec3{0.1f})));
        CHECK_FALSE(moved.contains({0.0f, 1.0f, 1.0f}));
    }

    TEST_CASE("frustum keeps what the camera sees and rejects what it does not") {
        const Frustum f = camera().frustum();
        CHECK(f.contains({0.0f, 0.0f, 0.0f}));
        CHECK_FALSE(f.contains({0.0f, 0.0f, 20.0f}));     // позади
        CHECK_FALSE(f.contains({100.0f, 0.0f, 0.0f}));    // далеко сбоку
        CHECK_FALSE(f.contains({0.0f, 0.0f, -500.0f}));   // за дальней плоскостью
        CHECK(f.intersects(Aabb::from_center({0.0f, 0.0f, 0.0f}, glm::vec3{1.0f})));
        CHECK(f.intersects(Aabb::from_center({8.0f, 0.0f, 0.0f}, glm::vec3{4.0f}))); // частично
        CHECK_FALSE(f.intersects(Aabb::from_center({0.0f, 0.0f, 30.0f}, glm::vec3{1.0f})));
        CHECK(f.intersects_sphere({0.0f, 0.0f, 10.5f}, 1.0f)); // касается ближней плоскости
    }
}

TEST_SUITE("MeshData") {
    TEST_CASE("box: 24 vertices, 12 triangles, outward faces, unit normals") {
        const MeshData box = MeshData::box({2.0f, 1.0f, 4.0f});
        CHECK(box.vertices.size() == 24);
        CHECK(box.triangle_count() == 12);
        CHECK(box.bounds() == Aabb{{-1.0f, -0.5f, -2.0f}, {1.0f, 0.5f, 2.0f}});
        CHECK(faces_outward(box));
        for (const Vertex3D& v : box.vertices) CHECK(glm::length(v.normal) == doctest::Approx(1.0f));
    }

    TEST_CASE("quad and plane: UV (0,0) is the top-left of the picture") {
        const MeshData quad = MeshData::quad({2.0f, 2.0f});
        CHECK(quad.vertices[0].position == glm::vec3{-1.0f, 1.0f, 0.0f}); // левый верх
        CHECK(quad.vertices[0].uv == glm::vec2{0.0f, 0.0f});
        CHECK(quad.vertices[0].normal == glm::vec3{0.0f, 0.0f, 1.0f});
        const MeshData plane = MeshData::plane({4.0f, 4.0f}, {4, 2});
        CHECK(plane.vertices.size() == 15);
        CHECK(plane.triangle_count() == 16);
        CHECK(plane.vertices[0].position.z == doctest::Approx(-2.0f)); // верх картинки — дальний край
        // Все треугольники плоскости смотрят вверх.
        for (std::size_t i = 0; i < plane.indices.size(); i += 3) {
            const glm::vec3 a = plane.vertices[plane.indices[i]].position;
            const glm::vec3 b = plane.vertices[plane.indices[i + 1]].position;
            const glm::vec3 c = plane.vertices[plane.indices[i + 2]].position;
            CHECK(glm::cross(b - a, c - a).y > 0.0f);
        }
    }

    TEST_CASE("sphere, cylinder and rounded slab are closed and face outward") {
        const MeshData sphere = MeshData::sphere(1.0f, 16, 8);
        CHECK(faces_outward(sphere));
        CHECK(sphere.bounds().size().x == doctest::Approx(2.0f).epsilon(1e-3));
        const MeshData cylinder = MeshData::cylinder(0.5f, 2.0f, 12);
        CHECK(faces_outward(cylinder));
        CHECK(cylinder.bounds().size().y == doctest::Approx(2.0f));
        const MeshData slab = MeshData::rounded_slab({1.4f, 2.0f}, 0.04f, 0.1f, 4);
        CHECK(faces_outward(slab));
        CHECK(slab.bounds().size().x == doctest::Approx(1.4f));
        CHECK(slab.bounds().size().z == doctest::Approx(0.04f));
    }

    TEST_CASE("rounded rect with radius = half size is a disc") {
        const MeshData disc = MeshData::rounded_rect({2.0f, 2.0f}, 1.0f, 8);
        for (std::size_t i = 1; i < disc.vertices.size(); ++i) {
            CHECK(glm::length(disc.vertices[i].position) == doctest::Approx(1.0f));
        }
    }

    TEST_CASE("append transforms positions and normals") {
        MeshData scene;
        scene.append(MeshData::quad(), glm::translate(glm::mat4{1.0f}, {5.0f, 0.0f, 0.0f}));
        scene.append(MeshData::quad(), glm::rotate(glm::mat4{1.0f}, std::numbers::pi_v<float>, glm::vec3{0.0f, 1.0f, 0.0f}));
        CHECK(scene.vertices.size() == 8);
        CHECK(scene.indices[6] == 4); // индексы второй сетки сдвинуты
        CHECK(scene.vertices[0].position.x == doctest::Approx(4.5f));
        CHECK(scene.vertices[4].normal.z == doctest::Approx(-1.0f));
    }

    TEST_CASE("compute_normals restores the normals of a flat quad") {
        MeshData quad = MeshData::quad();
        for (Vertex3D& v : quad.vertices) v.normal = {1.0f, 0.0f, 0.0f};
        quad.compute_normals();
        for (const Vertex3D& v : quad.vertices) CHECK(v.normal.z == doctest::Approx(1.0f));
    }
}

TEST_SUITE("Procedural") {
    TEST_CASE("noise is deterministic, bounded and depends on the seed") {
        const glm::vec3 p{1.3f, 2.7f, 0.5f};
        CHECK(Procedural::perlin(p) == Procedural::perlin(p));
        CHECK(Procedural::perlin(p, 1) != Procedural::perlin(p, 2));
        for (int i = 0; i < 200; ++i) {
            const glm::vec3 q{static_cast<float>(i) * 0.37f, static_cast<float>(i) * 0.11f, 0.5f};
            CHECK(std::abs(Procedural::perlin(q)) <= 1.1f);
            CHECK(Procedural::turbulence(q) >= 0.0f);
        }
    }

    TEST_CASE("gradient hits its stops exactly and clamps outside") {
        const Procedural::GradientStop stops[] = {{0.0f, Colors::black}, {0.5f, Colors::red}, {1.0f, Colors::white}};
        CHECK(Procedural::sample_gradient(stops, -1.0f) == Colors::black);
        CHECK(Procedural::sample_gradient(stops, 0.5f) == Colors::red);
        CHECK(Procedural::sample_gradient(stops, 2.0f) == Colors::white);
        CHECK(Procedural::sample_gradient(stops, 0.25f) == Color{128, 0, 0, 255});
    }

    TEST_CASE("noise image is reproducible") {
        const Procedural::NoiseImageDesc desc{.width = 32, .height = 16, .z = 3.0f};
        const Image a = Procedural::noise_image(desc, {{0.0f, Colors::black}, {1.0f, Colors::white}});
        const Image b = Procedural::noise_image(desc, {{0.0f, Colors::black}, {1.0f, Colors::white}});
        CHECK(a.width() == 32);
        CHECK(std::ranges::equal(a.pixels(), b.pixels()));
    }

    TEST_CASE("circle and ring images") {
        const Image circle = Procedural::circle_image(32, Colors::white);
        CHECK(circle.pixel(16, 16).a == 255);
        CHECK(circle.pixel(0, 0).a == 0);
        const Image ring = Procedural::circle_image(32, Colors::white, 3.0f);
        CHECK(ring.pixel(16, 16).a == 0);  // середина кольца пустая
        CHECK(ring.pixel(16, 1).a > 128);  // а край — нет
    }
}

TEST_SUITE("Image (stb_image_write)") {
    TEST_CASE("PNG round trip keeps every pixel") {
        Image image = Image::checkerboard(7, 5, 2, Colors::red, Color{0, 128, 255, 100});
        const std::vector<std::byte> png = image.encode_png();
        REQUIRE(png.size() > 8);
        CHECK(png[1] == std::byte{'P'});
        const auto decoded = Image::decode(png);
        REQUIRE(decoded.has_value());
        CHECK(std::ranges::equal(decoded->pixels(), image.pixels()));
    }

    TEST_CASE("save_png reports errors as data") {
        CHECK_FALSE(Image().save_png("never.png").has_value());
        CHECK_FALSE(Image(2, 2).save_png("/nonexistent-dir/x.png").has_value());
    }

    TEST_CASE("blend composes with alpha and clips at the border") {
        Image base(4, 4, Colors::black);
        Image top(2, 2, Color{255, 255, 255, 128});
        base.blend(top, 3, 3); // только один пиксель внутри
        CHECK(base.pixel(3, 3).r == 128);
        CHECK(base.pixel(3, 3).a == 255);
        CHECK(base.pixel(2, 2) == Colors::black);
    }

    TEST_CASE("Color helpers: lerp and scaled") {
        CHECK(Color::lerp(Colors::black, Colors::white, 0.5f) == Color{128, 128, 128, 255});
        CHECK(Color::lerp(Colors::black, Colors::white, 5.0f) == Colors::white);
        CHECK(Color{100, 50, 10, 77}.scaled(2.0f) == Color{200, 100, 20, 77});
        CHECK(Color{200, 0, 0, 255}.scaled(2.0f).r == 255);
    }
}
