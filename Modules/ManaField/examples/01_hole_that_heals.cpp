/**
 * @example 01_hole_that_heals.cpp
 * Поле маны: забрать ману из точки (`draw`) — в тумане остаётся дыра, которая затягивается диффузией и возвратом к базе.
 * Поле разреженное: пока никто не брал ману, оно не занимает память; чанки появляются и исчезают сами.
 */

#include <ManaField/ManaField.hpp>

#include <algorithm>
#include <cstdio>

using namespace ManaField;
using Math::Fixed;
using Math::Mana;
using Math::WorldPos;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/// Срез поля вокруг точки: цифра 0…9 — плотность относительно базы (9 — норма и больше).
static void print_slice(const ManaGrid& grid, CellPos centre, int radius) {
    for (int dz = -radius; dz <= radius; ++dz) {
        std::printf("     ");
        for (int dx = -radius; dx <= radius; ++dx) {
            const double ratio = (grid.at(centre.x + dx, centre.y, centre.z + dz) / grid.config().base).to_double(); // Mana / Mana → число
            std::printf("%d", static_cast<int>(std::min(ratio, 1.0) * 9.0 + 0.5));
        }
        std::printf("\n");
    }
}

int main() {
    ManaGrid grid; // конфигурация по умолчанию: база 40 на ячейку, диффузия и возврат подобраны под «дыру на 5–10 секунд»
    const WorldPos mage = WorldPos::from_meters(64, 10, 64);

    // 1. Новое поле — «виртуальная база»: ни одного чанка, везде базовая плотность.
    std::printf("1. новое поле: чанков %zu, плотность в точке мага %.0f\n", grid.allocated_chunks(), grid.density(mage).to_double());
    EXPECT(grid.allocated_chunks() == 0);

    // 2. Каст из окружения: маг забирает 240 маны из шара радиуса 3 м. Берётся пропорционально плотности ячеек.
    const Mana got = grid.draw(mage, Fixed::from_int(3), Mana::from_int(240));
    std::printf("2. забрали %.0f маны; чанков %zu; в центре осталось %.0f из %.0f\n", got.to_double(), grid.allocated_chunks(), grid.density(mage).to_double(),
                grid.config().base.to_double());
    std::printf("   срез (9 = норма):\n");
    print_slice(grid, cell_of(mage), 3);
    EXPECT(got == Mana::from_int(240));
    EXPECT(grid.excess_raw() == -got.raw()); // сумма «ячейка − база» ровно минус забранное

    // 3. Поле шагает 10 раз в секунду. Дыра затягивается: часть — диффузией из соседей, часть — возвратом к базе.
    const Mana base = grid.config().base;
    int steps = 0;
    while (grid.density(mage) < base * Fixed::from_ratio(9, 10)) {
        grid.step();
        ++steps;
    }
    std::printf("3. центр дыры вернулся до 90%% базы за %.1f с (%d шагов)\n   срез в этот момент:\n", steps / 10.0, steps);
    print_slice(grid, cell_of(mage), 3);
    EXPECT(steps / 10.0 >= 5.0 && steps / 10.0 <= 10.0); // дыра затягивается за 5–10 секунд — настраивается ManaField::Config

    // 4. Когда все ячейки вернулись к базе, чанки освобождаются: поле снова ничего не занимает.
    while (grid.allocated_chunks() > 0) grid.step();
    std::printf("4. после полного затягивания: чанков %zu, хеш равен хешу пустого поля: %s\n", grid.allocated_chunks(), grid.hash() == ManaGrid{}.hash() ? "да" : "нет");
    EXPECT(grid.hash() == ManaGrid{}.hash());

    // 5. Поле безгранично: далёкие и отрицательные координаты работают так же, чанки создаются только там, где нужно.
    const WorldPos far = WorldPos::from_meters(-5'000'000'000LL, 300, 7'000'000'000LL);
    (void)grid.draw(far, Fixed::from_int(3), Mana::from_int(50));
    std::printf("5. дыра в %.0f км от начала: чанков %zu, рядом с началом координат ничего не выделялось: %s\n", 5'000'000.0 * 1.0, grid.allocated_chunks(),
                grid.density(mage) == base ? "да" : "нет");
    EXPECT(grid.density(mage) == base);
    std::printf("OK\n");
    return 0;
}
