/**
 * @file test_core.cpp
 * @brief Тесты ядра без GPU: цвет, геометрия, изображения.
 */

#include <RendererSystem/RendererSystem.hpp>

#include <doctest/doctest.h>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

#include <cstddef>
#include <vector>

using namespace RendererSystem;

namespace {

/// Кодирует изображение в PNG в памяти (для проверки Image::decode).
std::vector<std::byte> encode_png(const Image& image) {
    std::vector<std::byte> out;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* buffer = static_cast<std::vector<std::byte>*>(context);
            const auto* bytes = static_cast<const std::byte*>(data);
            buffer->insert(buffer->end(), bytes, bytes + size);
        },
        &out, image.width(), image.height(), 4, image.pixels().data(), image.width() * 4);
    return out;
}

} // namespace

TEST_SUITE("Core::Color") {
    TEST_CASE("from_rgba / to_rgba round-trip") {
        constexpr Color orange = Color::from_rgba(0xFF8000C0);
        static_assert(orange.r == 255 && orange.g == 128 && orange.b == 0 && orange.a == 192);
        static_assert(orange.to_rgba() == 0xFF8000C0);
        CHECK(Color::from_rgba(0x12345678).to_rgba() == 0x12345678);
    }

    TEST_CASE("from_floats rounds and clamps") {
        CHECK(Color::from_floats(1.0f, 0.5f, 0.0f) == Color{255, 128, 0, 255});
        CHECK(Color::from_floats(2.0f, -1.0f, 0.0f, 0.0f) == Color{255, 0, 0, 0});
    }

    TEST_CASE("to_vec4, with_alpha, modulate") {
        CHECK(Colors::white.to_vec4().w == doctest::Approx(1.0f));
        CHECK(Colors::red.with_alpha(10).a == 10);
        CHECK(Colors::white.modulate(Colors::red) == Colors::red);
        CHECK(Color{128, 128, 128, 255}.modulate(Color{128, 255, 0, 255}) == Color{64, 128, 0, 255});
    }
}

TEST_SUITE("Core::Geometry") {
    TEST_CASE("Rect accessors and queries") {
        const Rect rect{{10.0f, 20.0f}, {30.0f, 40.0f}};
        CHECK(rect.max() == glm::vec2{40.0f, 60.0f});
        CHECK(rect.center() == glm::vec2{25.0f, 40.0f});
        CHECK(rect.contains({10.0f, 20.0f}));
        CHECK_FALSE(rect.contains({40.0f, 60.0f})); // правая/нижняя граница не входит
        CHECK(rect.intersects({{35.0f, 55.0f}, {10.0f, 10.0f}}));
        CHECK_FALSE(rect.intersects({{40.0f, 20.0f}, {10.0f, 10.0f}})); // касание
    }

    TEST_CASE("UvRect::from_pixels maps a sheet cell to 0..1") {
        const UvRect uv = UvRect::from_pixels({{16.0f, 0.0f}, {16.0f, 16.0f}}, {64.0f, 32.0f});
        CHECK(uv.min == glm::vec2{0.25f, 0.0f});
        CHECK(uv.max == glm::vec2{0.5f, 0.5f});
        CHECK(uv.size() == glm::vec2{0.25f, 0.5f});
    }
}

TEST_SUITE("Core::Image") {
    TEST_CASE("construction fills pixels") {
        const Image image(3, 2, Colors::blue);
        CHECK(image.width() == 3);
        CHECK(image.height() == 2);
        CHECK(image.pixels().size() == 6);
        CHECK(image.pixel(2, 1) == Colors::blue);
        CHECK(Image{}.empty());
        CHECK_THROWS_AS(Image(-1, 2), RendererError);
    }

    TEST_CASE("set_pixel, fill_rect with clipping") {
        Image image(4, 4, Colors::black);
        image.set_pixel(0, 0, Colors::red);
        image.fill_rect(2, 2, 10, 10, Colors::green); // выходит за край — обрезается
        CHECK(image.pixel(0, 0) == Colors::red);
        CHECK(image.pixel(3, 3) == Colors::green);
        CHECK(image.pixel(1, 1) == Colors::black);
    }

    TEST_CASE("checkerboard") {
        const Image image = Image::checkerboard(4, 4, 2, Colors::white, Colors::black);
        CHECK(image.pixel(0, 0) == Colors::white);
        CHECK(image.pixel(2, 0) == Colors::black);
        CHECK(image.pixel(2, 2) == Colors::white);
        CHECK_THROWS_AS((void)Image::checkerboard(4, 4, 0, Colors::white, Colors::black), RendererError);
    }

    TEST_CASE("flip_vertically swaps rows") {
        Image image(2, 3, Colors::black);
        image.set_pixel(1, 0, Colors::red);
        image.flip_vertically();
        CHECK(image.pixel(1, 2) == Colors::red);
        CHECK(image.pixel(1, 0) == Colors::black);
    }

    TEST_CASE("decode PNG keeps top row first and RGBA values") {
        Image source(3, 2, Colors::transparent);
        source.set_pixel(0, 0, Colors::red);                // верхний левый
        source.set_pixel(2, 1, Color{10, 20, 30, 128});    // нижний правый, полупрозрачный

        const auto png = encode_png(source);
        REQUIRE_FALSE(png.empty());

        const auto decoded = Image::decode(png);
        REQUIRE(decoded.has_value());
        CHECK(decoded->width() == 3);
        CHECK(decoded->height() == 2);
        CHECK(decoded->pixel(0, 0) == Colors::red);
        CHECK(decoded->pixel(2, 1) == Color{10, 20, 30, 128});
    }

    TEST_CASE("decode and load report errors") {
        const std::byte garbage[] = {std::byte{1}, std::byte{2}, std::byte{3}};
        const auto bad = Image::decode(garbage);
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().find("cannot decode") != std::string::npos);

        CHECK_FALSE(Image::decode({}).has_value());

        const auto missing = Image::load("definitely/missing/file.png");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().find("cannot open") != std::string::npos);
    }
}
