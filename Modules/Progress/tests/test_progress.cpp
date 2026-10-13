#include <Progress/Progress.hpp>

#include <doctest/doctest.h>

using namespace Progress;
using Runes::Rune;
using Math::Fixed;
using Math::Mana;

TEST_CASE("таблица по умолчанию: пороги опыта, названия, суммарная прибавка маны") {
    const RankTable t = RankTable::default_table();
    REQUIRE(t.rank_count() == 4);
    CHECK(t.rank_for_xp(0) == 0);
    CHECK(t.rank_for_xp(99) == 0);
    CHECK(t.rank_for_xp(100) == 1);
    CHECK(t.rank_for_xp(699) == 2);
    CHECK(t.rank_for_xp(700) == 3);
    CHECK(t.rank_for_xp(4'000'000'000u) == 3);
    CHECK(t.at(1).title == "Ученик");
    CHECK(t.xp_for_next(0) == 100u);
    CHECK(t.xp_for_next(3) == std::nullopt);
    CHECK(t.mana_bonus(0) == Mana::from_int(0));
    CHECK(t.mana_bonus(2) == Mana::from_int(200));
    CHECK(t.mana_bonus(3) == Mana::from_int(400));
}

TEST_CASE("руны открываются рангами; руна, не названная в таблице, доступна с начала") {
    const RankTable t = RankTable::default_table();
    CHECK(t.rank_of_rune(Rune::Carve) == 0);
    CHECK(t.rank_of_rune(Rune::Sub) == 0);
    CHECK(t.rank_of_rune(Rune::Lt) == 1);
    CHECK(t.rank_of_rune(Rune::Jmp) == 1);
    CHECK(t.rank_of_rune(Rune::Store) == 2);
    CHECK(t.rank_of_rune(Rune::Call) == 3);
    CHECK_FALSE(t.allowed(0, Rune::Call));
    CHECK(t.allowed(3, Rune::Call));
    CHECK(t.allowed(3, Rune::Carve));
}

TEST_CASE("first_locked: первая закрытая руна программы, пока ранг не вырос") {
    const RankTable t = RankTable::default_table();
    const auto p = Runes::parse_program("PUSH 1\nSTORE 0\nCALL f\nHALT\nf:\nRET\n", "t");
    REQUIRE(p.has_value());
    CHECK(t.first_locked(*p, 0) == Rune::Store);
    CHECK(t.first_locked(*p, 1) == Rune::Store);
    CHECK(t.first_locked(*p, 2) == Rune::Call);
    CHECK(t.first_locked(*p, 3) == std::nullopt);
    const auto basic = Runes::parse_program("TARGET\nPUSH 2\nCARVE\nHALT\n", "t");
    REQUIRE(basic.has_value());
    CHECK(t.first_locked(*basic, 0) == std::nullopt);
}

TEST_CASE("award: опыт копится, ранг растёт и не падает, переполнения нет") {
    const RankTable t = RankTable::default_table();
    Mastery m;
    CHECK_FALSE(award(t, m, 99).ranked_up());
    const Award up = award(t, m, 1);
    CHECK(up.ranked_up());
    CHECK(up.old_rank == 0);
    CHECK(up.new_rank == 1);
    CHECK(m.xp == 100);
    const Award jump = award(t, m, 1000); // сразу через два ранга
    CHECK(jump.old_rank == 1);
    CHECK(jump.new_rank == 3);
    m.xp = 4'294'967'290u;
    (void)award(t, m, 100);
    CHECK(m.xp == 4'294'967'295u);
    CHECK(m.rank == 3);
}

TEST_CASE("effect_xp: 1 + r² по целым метрам; крупные эффекты ценнее") {
    CHECK(effect_xp(Fixed::from_ratio(1, 4)) == 1);
    CHECK(effect_xp(Fixed::from_int(1)) == 2);
    CHECK(effect_xp(Fixed::from_int(3)) == 10);
    CHECK(effect_xp(Fixed::from_int(6)) == 37);
    CHECK(effect_xp(Fixed::from_int(-2)) == 1);
}

TEST_CASE("своя таблица: данные заменяют код; хеш отличает состояния") {
    const RankTable one({{0, "Единственный", {Rune::Carve}, Mana::from_int(5)}});
    CHECK(one.rank_count() == 1);
    CHECK(one.rank_of_rune(Rune::Carve) == 0);
    Mastery a, b;
    b.xp = 1;
    Math::Hasher ha, hb;
    hash_mastery(a, ha);
    hash_mastery(b, hb);
    CHECK(ha.value() != hb.value());
}
