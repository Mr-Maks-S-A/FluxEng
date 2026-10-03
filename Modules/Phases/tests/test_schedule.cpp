#include <Phases/Schedule.hpp>

#include <doctest/doctest.h>

#include <string>

using Phases::Schedule;

TEST_CASE("фазы выполняются по порядку добавления") {
    std::string log;
    Schedule s;
    s.add("a", [&] { log += 'a'; }).add("b", [&] { log += 'b'; }).add("c", [&] { log += 'c'; });
    s.run();
    CHECK(log == "abc");
    CHECK(s.size() == 3);
    const auto names = s.names();
    REQUIRE(names.size() == 3);
    CHECK(names[1] == "b");
}

TEST_CASE("вставка до и после якоря, чужой код не правится") {
    std::string log;
    Schedule s;
    s.add("commands", [&] { log += 'C'; }).add("movement", [&] { log += 'M'; }).add("events", [&] { log += 'E'; });
    CHECK(s.insert_after("movement", "machines", [&] { log += 'X'; }));
    CHECK(s.insert_before("commands", "input", [&] { log += 'I'; }));
    CHECK_FALSE(s.insert_after("nowhere", "ghost", [&] { log += '?'; }));
    CHECK_FALSE(s.contains("ghost"));
    s.run();
    CHECK(log == "ICMXE");
}

TEST_CASE("remove и set_enabled") {
    std::string log;
    Schedule s;
    s.add("a", [&] { log += 'a'; }).add("b", [&] { log += 'b'; }).add("c", [&] { log += 'c'; });
    CHECK(s.set_enabled("b", false));
    s.run();
    CHECK(log == "ac");
    CHECK(s.times()[1].milliseconds == 0.0); // выключенная фаза не меряется
    CHECK(s.set_enabled("b", true));
    CHECK(s.remove("a"));
    CHECK_FALSE(s.remove("a"));
    CHECK_FALSE(s.set_enabled("a", true));
    log.clear();
    s.run();
    CHECK(log == "bc");
}

TEST_CASE("замеры: по фазе, в порядке расписания, сумма") {
    Schedule s;
    volatile long sink = 0;
    s.add("fast", [] {}).add("slow", [&] {
        for (long i = 0; i < 2'000'000; ++i) sink = sink + i;
    });
    s.run();
    REQUIRE(s.times().size() == 2);
    CHECK(s.times()[0].name == "fast");
    CHECK(s.times()[1].name == "slow");
    CHECK(s.times()[1].milliseconds > s.times()[0].milliseconds);
    CHECK(s.total_ms() == doctest::Approx(s.times()[0].milliseconds + s.times()[1].milliseconds));
}

TEST_CASE("имена в times() остаются валидными после изменения расписания") {
    Schedule s;
    s.add("a", [] {}).add("b", [] {});
    s.insert_before("a", "z", [] {});
    s.remove("b");
    s.run();
    REQUIRE(s.times().size() == 2);
    CHECK(s.times()[0].name == "z");
    CHECK(s.times()[1].name == "a");
}

TEST_CASE("фаза может менять состояние, общее с другими фазами: порядок виден в результате") {
    int value = 1;
    Schedule s;
    s.add("double", [&] { value *= 2; }).add("add", [&] { value += 3; });
    s.run();
    CHECK(value == 5); // (1·2)+3
    Schedule reversed;
    int other = 1;
    reversed.add("add", [&] { other += 3; }).add("double", [&] { other *= 2; });
    reversed.run();
    CHECK(other == 8); // (1+3)·2 — порядок фаз определяет результат: поэтому он данные, а не случайность вызовов
}
