/**
 * @example 01_fixed_and_world.cpp
 * Числа симуляции: Fixed (Q16.16), WorldPos (int64), сильный тип Mana и граница с float.
 *
 * Главное правило: симуляция считает только в целых числах и Fixed — тогда результат побитово
 * одинаков на любой машине и в любой сборке. float живёт только на границе (ввод, камера, отображение).
 */

#include <Math/Math.hpp>

#include <cstdio>

using namespace Math;
using namespace Math::literals; // 1.5_fx, 240_mana

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    // 1. Fixed: 16 бит дробной части на int32. Умножение и деление идут через int64 и округляют к нулю.
    const Fixed a = 1.5_fx, b = Fixed::from_ratio(1, 3);
    std::printf("1. 1.5 · 1/3 = %.5f (raw %d); 1.5 + 1/3 = %.5f\n", (a * b).to_double(), (a * b).raw, (a + b).to_double());
    EXPECT(a * b == Fixed::from_ratio(1, 2) || (a * b).raw == 32767); // 0.4999…: округление к нулю
    EXPECT((-a) * b == -(a * b));                                    // симметрично: потоки маны взаимно гасятся

    // 2. Насыщение вместо переполнения и деление на ноль без падения: симуляция не должна падать от плохих данных.
    EXPECT(Fixed::max() + 1_fx == Fixed::max());
    EXPECT(1_fx / Fixed{} == Fixed::max());
    std::printf("2. max + 1 = max, 1/0 = max: симуляция не падает\n");

    // 3. Корень и длина — целочисленные (без float).
    EXPECT(sqrt(2_fx).to_double() > 1.4141 && sqrt(2_fx).to_double() < 1.4143);
    EXPECT(length(FVec3{3_fx, 4_fx, 0_fx}) == 5_fx);
    const FVec3 up = normalize({0_fx, 7_fx, 0_fx});
    std::printf("3. √2 = %.5f, |(3,4,0)| = %.1f, normalize(0,7,0) = (%.1f, %.1f, %.1f)\n", sqrt(2_fx).to_double(), 5.0, up.x.to_double(), up.y.to_double(),
                up.z.to_double());

    // 4. WorldPos: int64 в 1/65536 м — диапазон около 940 а.е. при точности 15 мкм. Солнечная система без смены формата.
    const WorldPos far_away = WorldPos::from_meters(134'000'000'000'000LL, 0, 0); // 134 млн млн м ≈ 900 а.е.
    EXPECT(far_away.x > 0);
    const WorldPos step = advance(WorldPos::from_meters(1, 2, 3), FVec3{2_fx, 0_fx, -1_fx}, 0.5_fx); // pos + v · 0.5
    std::printf("4. шаг: (%.1f, %.1f, %.1f) м\n", step.to_doubles()[0], step.to_doubles()[1], step.to_doubles()[2]);
    EXPECT(step.to_doubles()[0] == 2.0);

    // 5. Граница с float: ровно в одном месте, ввод превращается в команды. Симуляция эти функции не вызывает.
    const Fixed from_mouse = Fixed::from_double(0.7071);
    const FVec3 look = quantize_direction(1.0, -1.0, 0.0); // направление взгляда → единичный FVec3
    std::printf("5. 0.7071 → raw %d; взгляд (1,-1,0) → (%.4f, %.4f, %.4f)\n", from_mouse.raw, look.x.to_double(), look.y.to_double(), look.z.to_double());
    EXPECT(from_mouse.raw == 46341); // 0.7071 · 65536 = 46341.2, округление к ближайшему

    // 6. Mana — тот же Fixed, но другой тип: ману нельзя сложить с метрами и передать как радиус.
    const Mana cost = 240_mana, wallet = Mana::from_int(1000);
    const Mana left = wallet - cost;
    const Fixed ratio = cost / wallet; // Mana / Mana → безразмерное число
    std::printf("6. после траты %.0f из %.0f осталось %.0f (%.0f%%)\n", cost.to_double(), wallet.to_double(), left.to_double(), ratio.to_double() * 100.0);
    EXPECT(left == 760_mana);
    // Mana + Fixed{} — не компилируется (проверено в тестах): единицы не смешиваются.

    // 7. Гравитация — функция от позиции: сейчас константа, потом планеты.
    std::printf("7. g = %.2f м/с²\n", gravity({}).y.to_double());
    EXPECT(gravity({}).y.to_double() < -9.8);
    std::printf("OK\n");
    return 0;
}
