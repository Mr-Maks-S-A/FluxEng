#include <AudioSystem/AudioSystem.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace AudioSystem;

namespace {
Spatial at(Vec3 position) {
    Spatial s;
    s.enabled = true;
    s.position = position;
    return s;
}
} // namespace

TEST_SUITE("AudioSystem.Spatial") {

TEST_CASE("затухание: модели в контрольных точках") {
    // min = 1, max = 11
    CHECK(attenuation(AttenuationModel::None, 100.0f, 1.0f, 11.0f, 1.0f) == 1.0f);

    CHECK(attenuation(AttenuationModel::Inverse, 0.5f, 1.0f, 11.0f, 1.0f) == 1.0f); // ближе min — полная
    CHECK(attenuation(AttenuationModel::Inverse, 2.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(0.5f));
    CHECK(attenuation(AttenuationModel::Inverse, 3.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(1.0f / 3.0f));
    CHECK(attenuation(AttenuationModel::Inverse, 500.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(1.0f / 11.0f)); // дальше max не падает

    CHECK(attenuation(AttenuationModel::Linear, 1.0f, 1.0f, 11.0f, 1.0f) == 1.0f);
    CHECK(attenuation(AttenuationModel::Linear, 6.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(0.5f));
    CHECK(attenuation(AttenuationModel::Linear, 11.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(0.0f));
    CHECK(attenuation(AttenuationModel::Linear, 99.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(0.0f));

    CHECK(attenuation(AttenuationModel::Exponential, 4.0f, 1.0f, 11.0f, 1.0f) == doctest::Approx(0.25f));
    CHECK(attenuation(AttenuationModel::Exponential, 4.0f, 1.0f, 11.0f, 2.0f) == doctest::Approx(1.0f / 16.0f));
}

TEST_CASE("затухание: мусорные параметры не дают NaN, Inf и усиления") {
    for (const AttenuationModel m : {AttenuationModel::Inverse, AttenuationModel::Linear, AttenuationModel::Exponential}) {
        for (const float d : {0.0f, 1e-9f, 1.0f, 1e9f}) {
            for (const float min_d : {-1.0f, 0.0f, 1.0f}) {
                for (const float rolloff : {-1.0f, 0.0f, 3.0f}) {
                    const float a = attenuation(m, d, min_d, 0.0f, rolloff);
                    CHECK(std::isfinite(a));
                    CHECK(a >= 0.0f);
                    CHECK(a <= 1.0f + 1e-6f);
                }
            }
        }
    }
}

TEST_CASE("пан: справа — вправо, слева — влево, спереди и сзади — центр") {
    Listener l; // смотрит в −Z, вверх +Y, право +X
    CHECK(compute_spatial(at({10, 0, 0}), l).pan > 0.99f);
    CHECK(compute_spatial(at({-10, 0, 0}), l).pan < -0.99f);
    CHECK(std::abs(compute_spatial(at({0, 0, -10}), l).pan) < 1e-5f);
    CHECK(std::abs(compute_spatial(at({0, 0, 10}), l).pan) < 1e-5f);
    CHECK(std::abs(compute_spatial(at({0, 10, 0}), l).pan) < 1e-5f);
}

TEST_CASE("пан следует за поворотом слушателя") {
    Listener l;
    l.forward = {1, 0, 0}; // повернулся направо; право теперь +Z
    CHECK(compute_spatial(at({0, 0, 10}), l).pan > 0.99f);
    CHECK(compute_spatial(at({0, 0, -10}), l).pan < -0.99f);
}

TEST_CASE("источник в голове слушателя: центр, полная громкость, всё конечно") {
    const SpatialResult r = compute_spatial(at({0, 0, 0}), Listener{});
    CHECK(r.pan == 0.0f);
    CHECK(r.gain == 1.0f);
    CHECK(r.pitch == 1.0f);
    CHECK(std::isfinite(r.cutoff_hz));
}

TEST_CASE("вырожденный слушатель (нулевое направление) не даёт NaN") {
    Listener l;
    l.forward = {0, 0, 0};
    l.up = {0, 0, 0};
    const SpatialResult r = compute_spatial(at({3, 4, 5}), l);
    CHECK(std::isfinite(r.pan));
    CHECK(std::isfinite(r.gain));
}

TEST_CASE("конус: внутри — полная громкость, за внешним углом — outer_gain, между — плавно") {
    Spatial s = at({0, 0, 0});
    s.direction = {0, 0, 1};
    s.cone_inner_degrees = 60.0f;
    s.cone_outer_degrees = 180.0f;
    s.cone_outer_gain = 0.2f;
    s.min_distance = 100.0f; // затухание по расстоянию не мешает
    Listener l;
    l.position = {0, 0, 10}; // прямо по оси источника
    CHECK(compute_spatial(s, l).gain == doctest::Approx(1.0f));
    l.position = {0, 0, -10}; // за спиной источника
    CHECK(compute_spatial(s, l).gain == doctest::Approx(0.2f));
    l.position = {10, 0, 0}; // под 90°: между внутренним полуутлом 30° и внешним 90° → ровно outer
    CHECK(compute_spatial(s, l).gain == doctest::Approx(0.2f));
    l.position = {10, 0, 10}; // 45°: (45−30)/(90−30) = 0.25 пути к outer
    CHECK(compute_spatial(s, l).gain == doctest::Approx(1.0f + (0.2f - 1.0f) * 0.25f).epsilon(0.01));
}

TEST_CASE("Доплер: сближение повышает тон, удаление понижает, покой не меняет") {
    Listener l;
    Spatial s = at({0, 0, -100});
    s.velocity = {0, 0, 50}; // летит к слушателю
    const float approaching = compute_spatial(s, l).pitch;
    s.velocity = {0, 0, -50};
    const float receding = compute_spatial(s, l).pitch;
    s.velocity = {};
    const float still = compute_spatial(s, l).pitch;
    CHECK(approaching > 1.1f);
    CHECK(receding < 0.9f);
    CHECK(still == 1.0f);
    CHECK(approaching == doctest::Approx(343.0f / (343.0f - 50.0f)));
    s.velocity = {0, 0, 50};
    s.doppler = 0.0f;
    CHECK(compute_spatial(s, l).pitch == 1.0f);

    // слушатель едет к неподвижному источнику
    s.doppler = 1.0f;
    s.velocity = {};
    l.velocity = {0, 0, -50};
    CHECK(compute_spatial(s, l).pitch == doctest::Approx((343.0f + 50.0f) / 343.0f));
}

TEST_CASE("Доплер ограничен: сверхзвуковая скорость не ломает высоту") {
    Listener l;
    Spatial s = at({0, 0, -100});
    s.velocity = {0, 0, 10'000};
    const float pitch = compute_spatial(s, l).pitch;
    CHECK(pitch <= 2.0f);
    CHECK(pitch >= 0.5f);
}

TEST_CASE("воздух и препятствия глушат высокие и тише") {
    Spatial s = at({0, 0, -50});
    Listener l;
    CHECK(compute_spatial(s, l).cutoff_hz == 20'000.0f);
    s.air_absorption = 0.05f;
    const float near_cutoff = compute_spatial(at({0, 0, -2}), l).cutoff_hz;
    const float far_cutoff = compute_spatial(s, l).cutoff_hz;
    CHECK(far_cutoff < 20'000.0f);
    CHECK(far_cutoff >= 800.0f);
    CHECK(near_cutoff == 20'000.0f); // air_absorption у ближнего не задана

    Spatial wall = at({0, 0, -5});
    const SpatialResult open = compute_spatial(wall, l);
    wall.occlusion = 1.0f;
    const SpatialResult blocked = compute_spatial(wall, l);
    CHECK(blocked.gain < open.gain);
    CHECK(blocked.cutoff_hz == doctest::Approx(800.0f));
}

} // TEST_SUITE
