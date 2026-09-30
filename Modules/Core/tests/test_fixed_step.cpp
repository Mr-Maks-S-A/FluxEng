#include <Core/FixedStep.hpp>

#include <doctest/doctest.h>

using Core::FixedStep;

TEST_CASE("FixedStep: целое число тиков, остаток переносится в следующий кадр") {
    FixedStep step;
    step.ticks_per_second = 4.0; // тик = 0.25 с: точно представимо в double

    CHECK(step.advance(0.625) == 2);
    CHECK(step.alpha() == doctest::Approx(0.5));
    CHECK(step.advance(0.125) == 1); // 0.125 + 0.125 остатка = ровно тик
    CHECK(step.alpha() == doctest::Approx(0.0));
}

TEST_CASE("FixedStep: скорость умножает время кадра") {
    FixedStep step;
    step.ticks_per_second = 4.0;
    step.speed = 4;
    CHECK(step.advance(0.25) == 4);
}

TEST_CASE("FixedStep: пауза останавливает тики и не копит время") {
    FixedStep step;
    step.ticks_per_second = 10.0;
    step.paused = true;
    CHECK(step.advance(1.0) == 0);
    step.paused = false;
    CHECK(step.advance(0.0) == 0);
}

TEST_CASE("FixedStep: медленный кадр ограничен и не догоняется бесконечно") {
    FixedStep step;
    step.ticks_per_second = 100.0;
    step.max_ticks_per_frame = 16;
    CHECK(step.advance(1.0) == 16);
    CHECK(step.alpha() == 0.0f);   // остаток сброшен
    CHECK(step.advance(0.0) == 0);
}

TEST_CASE("FixedStep: lockstep — ровно один тик на кадр при любом времени") {
    FixedStep step;
    step.lockstep = true;
    CHECK(step.advance(0.0) == 1);
    CHECK(step.advance(10.0) == 1);
    CHECK(step.alpha() == 0.0f);
}
