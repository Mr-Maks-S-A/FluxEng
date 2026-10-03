#include <RuneEditor/View.hpp>

#include <doctest/doctest.h>

using namespace RuneEditor;
using Runes::Rune;

TEST_CASE("View: каждая руна имеет род, цвета родов различаются") {
    CHECK(family_of(Rune::Carve) == Family::Effect);
    CHECK(family_of(Rune::JmpIf) == Family::Control);
    CHECK(family_of(Rune::Target) == Family::Context);
    CHECK(family_of(Rune::ManaAt) == Family::Sense);
    CHECK_FALSE(family_color(Family::Effect) == family_color(Family::Data));
    for (int r = 0; r < static_cast<int>(Rune::Count); ++r) CHECK_FALSE(glyph_label({.rune = static_cast<Rune>(r)}).empty());
}

TEST_CASE("View: подпись PUSH показывает число") {
    GraphNode n;
    n.rune = Rune::Push;
    n.value = 5 << 15; // 2.5
    CHECK(glyph_label(n).contains("2.5"));
}
