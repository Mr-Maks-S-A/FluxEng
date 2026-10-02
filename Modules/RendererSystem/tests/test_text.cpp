/**
 * @file test_text.cpp
 * @brief Шрифты без GPU: UTF-8, встроенный шрифт, раскладка (перенос, выравнивание), TTF системы с кириллицей.
 */

#include <RendererSystem/Text/Font.hpp>

#include <doctest/doctest.h>

#include <string_view>
#include <vector>

using namespace RendererSystem;

namespace {

std::vector<std::uint32_t> decode(std::string_view text) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < text.size();) out.push_back(next_codepoint(text, i));
    return out;
}

} // namespace

TEST_SUITE("UTF-8") {
    TEST_CASE("decodes 1-, 2-, 3- and 4-byte sequences") {
        CHECK(decode("A") == std::vector<std::uint32_t>{0x41});
        CHECK(decode("Ж") == std::vector<std::uint32_t>{0x416});
        CHECK(decode("—") == std::vector<std::uint32_t>{0x2014});
        CHECK(decode("\xF0\x9F\x99\x82") == std::vector<std::uint32_t>{0x1F642});
        CHECK(decode("Ход 1") == std::vector<std::uint32_t>{0x425, 0x43E, 0x434, 0x20, 0x31});
    }

    TEST_CASE("broken sequences become U+FFFD and decoding continues") {
        CHECK(decode("\x80" "A") == std::vector<std::uint32_t>{0xFFFD, 0x41});
        CHECK(decode("\xD0") == std::vector<std::uint32_t>{0xFFFD});       // обрыв в конце
        CHECK(decode("\xE2\x80" "A").back() == 0x41);
    }
}

TEST_SUITE("Font::builtin (stb_easy_font)") {
    TEST_CASE("has printable ASCII, metrics and a '?' fallback") {
        const Font font = Font::builtin(2);
        CHECK(font.glyph_count() == 95);
        CHECK(font.source() == "builtin");
        CHECK(font.line_height() == doctest::Approx(24.0f));
        REQUIRE(font.find('A') != nullptr);
        CHECK(font.find('A')->advance > 0.0f);
        CHECK(font.find(0x416) == nullptr);                 // кириллицы во встроенном шрифте нет…
        CHECK(font.glyph(0x416).codepoint == '?');          // …рисуется вопрос
        CHECK(font.atlas().width() == 512);
        // В атласе есть «чернила»: глиф A не пустой.
        const Glyph& a = font.glyph('A');
        int ink = 0;
        for (int y = 0; y < static_cast<int>(a.atlas.size.y); ++y)
            for (int x = 0; x < static_cast<int>(a.atlas.size.x); ++x)
                ink += font.atlas().pixel(static_cast<int>(a.atlas.position.x) + x, static_cast<int>(a.atlas.position.y) + y).a > 0;
        CHECK(ink > 10);
    }

    TEST_CASE("layout: width is the sum of advances, size scales everything") {
        const Font font = Font::builtin(1);
        const float ab = font.glyph('A').advance + font.glyph('B').advance;
        CHECK(font.measure("AB").x == doctest::Approx(ab));
        CHECK(font.measure("AB", {.size = font.pixel_height() * 3.0f}).x == doctest::Approx(ab * 3.0f));
        std::vector<GlyphQuad> quads;
        font.layout("A B", {}, quads);
        CHECK(quads.size() == 2); // пробел не рисуется
        CHECK(quads[1].rect.position.x > quads[0].rect.position.x);
    }

    TEST_CASE("layout: newlines and word wrap produce lines; alignment shifts them") {
        const Font font = Font::builtin(1);
        const float one_line = font.measure("aaa").y;
        CHECK(font.measure("aaa\nbbb").y == doctest::Approx(one_line + font.line_height()));

        const float word = font.measure("aaaa").x;
        const glm::vec2 wrapped = font.measure("aaaa aaaa aaaa", {.max_width = word * 1.5f});
        CHECK(wrapped.y == doctest::Approx(one_line + 2.0f * font.line_height())); // три строки

        std::vector<GlyphQuad> left, centered;
        font.layout("a", {.max_width = 100.0f}, left);
        font.layout("a", {.align = TextAlign::Center, .max_width = 100.0f}, centered);
        CHECK(centered[0].rect.position.x == doctest::Approx(left[0].rect.position.x + (100.0f - font.measure("a").x) * 0.5f));
    }

    TEST_CASE("a word longer than the line stays whole") {
        const Font font = Font::builtin(1);
        const glm::vec2 size = font.measure("abcdefghij", {.max_width = 5.0f});
        CHECK(size.x == doctest::Approx(font.measure("abcdefghij").x));
        CHECK(size.y == doctest::Approx(font.measure("a").y));
    }
}

TEST_SUITE("Font (stb_truetype)") {
    TEST_CASE("bad data is reported, not thrown") {
        const std::byte junk[16]{};
        CHECK_FALSE(Font::from_memory(junk).has_value());
        CHECK_FALSE(Font::from_memory({}).has_value());
        CHECK_FALSE(Font::load("definitely-missing.ttf").has_value());
    }

    TEST_CASE("system TTF bakes Cyrillic into a packed atlas") {
        auto font = Font::load_system({.pixel_height = 24.0f});
        if (!font) {
            MESSAGE("no system font: " << font.error() << " — test skipped");
            return;
        }
        CHECK(font->glyph_count() > 150);
        REQUIRE(font->find(0x416) != nullptr); // Ж
        CHECK(font->find(0x416)->size.x > 0.0f);
        CHECK(font->ascent() > 0.0f);
        CHECK(font->descent() < 0.0f);
        CHECK(font->line_height() >= font->ascent() - font->descent());
        CHECK(font->measure("Привет").x > font->measure("При").x);
        // Глифы в атласе не перекрываются (stb_rect_pack).
        const Glyph& a = *font->find(0x416);
        const Glyph& b = *font->find('W');
        CHECK_FALSE(a.atlas.intersects(b.atlas));
    }
}
