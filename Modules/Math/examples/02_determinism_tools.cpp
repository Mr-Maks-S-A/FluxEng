/**
 * @example 02_determinism_tools.cpp
 * Инструменты детерминизма: генератор с сидом, хеш состояния, шум на Fixed, поля расстояний (SdfField) и лучи.
 *
 * Идея: «один сид — один мир», а совпадение хешей двух прогонов доказывает, что симуляция не разошлась.
 */

#include <Math/Math.hpp>

#include <cstdio>

using namespace Math;
using namespace Math::literals;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    // 1. Rng: единственный источник случайности симуляции. Один сид — одна последовательность.
    Rng a(42), b(42);
    std::printf("1. сид 42: %llu %llu — и у второго генератора то же\n", static_cast<unsigned long long>(a.next()), static_cast<unsigned long long>(a.next()));
    EXPECT(b.next() == Rng(42).next());
    EXPECT(Rng(1).below(10) < 10);

    // 2. Hasher: контрольная сумма состояния. Добавляйте всё, что должно совпасть между прогонами.
    const auto world_hash = [](std::uint64_t seed) {
        Rng rng(seed);
        Hasher h;
        for (int i = 0; i < 1000; ++i) h.add(rng.next()); // «состояние» мира
        return h.value();
    };
    std::printf("2. хеш мира: %016llx (повтор: %016llx)\n", static_cast<unsigned long long>(world_hash(7)), static_cast<unsigned long long>(world_hash(7)));
    EXPECT(world_hash(7) == world_hash(7));
    EXPECT(world_hash(7) != world_hash(8));

    // 3. Шум высот на Fixed (value noise с октавами): рельеф без единого float.
    std::printf("3. высоты на сиде 5:");
    for (int x = 0; x < 6; ++x) std::printf(" %.2f", fbm(5, Fixed::from_int(x * 3), Fixed::from_int(2), 4).to_double());
    std::printf("\n");
    EXPECT(fbm(5, 3_fx, 4_fx, 4) == fbm(5, 3_fx, 4_fx, 4));
    EXPECT(fbm(5, 3_fx, 4_fx, 4) < 1_fx);

    // 4. SdfField — интерфейс «расстояние до поверхности»: плоскость, шар, ландшафт реализуют его одинаково.
    const PlaneSdf ground(WorldPos::from_meters(0, 10, 0).y); // земля на высоте 10 м
    const SphereSdf planet(WorldPos::from_meters(0, 0, 0), 100_fx);
    std::printf("4. над плоскостью на 2 м: %.1f; на 150 м от центра шара: %.1f\n", ground.sample(WorldPos::from_meters(0, 12, 0)).to_double(),
                planet.sample(WorldPos::from_meters(0, 150, 0)).to_double());
    EXPECT(ground.sample(WorldPos::from_meters(0, 12, 0)) == 2_fx);
    EXPECT(planet.sample(WorldPos::from_meters(0, 150, 0)) == 50_fx);

    // 5. Луч по полю: шагает на длину расстояния до поверхности. Работает с любым SdfField.
    const auto hit = raycast(planet, WorldPos::from_meters(0, 300, 0), {0_fx, -1_fx, 0_fx}, 500_fx);
    EXPECT(hit.has_value());
    std::printf("5. луч вниз попал в планету на %.1f м от старта (ожидалось 200)\n", hit->distance.to_double());
    EXPECT(hit->distance.to_double() > 199.0 && hit->distance.to_double() < 201.0);
    const FVec3 normal = planet.gradient(hit->position); // нормаль «наружу»
    EXPECT(normal.y.to_double() > 0.99);

    // 6. FLUX_ASSERT с обработчиком: перед abort() можно сбросить журнал (Replay::FlightRecorder делает это сам).
    const AssertHandler previous = set_assert_handler([](const char*, const char* message, const char*, int) { std::printf("   (обработчик ассерта: %s)\n", message); });
    set_assert_handler(previous); // снимаем: в примере ассерт не срабатывает
    std::printf("6. обработчик ассерта ставится и снимается\nOK\n");
    return 0;
}
