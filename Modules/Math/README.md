# Math — числа детерминированной симуляции

Всё, из чего симуляция считает: `Fixed` (Q16.16), `WorldPos` (int64), целочисленный корень, генератор с сидом, хеш состояния,
шум, интерфейс «расстояние до поверхности» (`SdfField`) и сильный тип `Mana`. **Никакого float внутри симуляции** — тогда результат
побитово одинаков на любой машине, а совпадение хешей двух прогонов доказывает отсутствие расхождения.

Подключение: `#include <Math/Math.hpp>`, цель CMake `engine::Math`. Зависимостей нет.

## Что где

| Тип / функция | Зачем |
|---|---|
| `Fixed` | число Q16.16 на int32; `*` и `/` через int64 с округлением к нулю, переполнение насыщается, `x / 0` не падает |
| `FVec3`, `WorldPos` | вектор из Fixed и позиция в мире (int64, 1/65536 м: ≈ 940 а.е. при 15 мкм) |
| `Mana` | то же `Fixed`, но другой тип: ману нельзя сложить с метрами и передать как радиус |
| `isqrt`, `sqrt`, `length`, `normalize` | целочисленные, без float |
| `Rng`, `mix64` | единственный источник случайности (SplitMix64 с сидом) |
| `Hasher` | FNV-1a 64: хеш состояния для сравнения прогонов |
| `fbm`, `value_noise` | шум высот на Fixed |
| `SdfField`, `PlaneSdf`, `SphereSdf`, `raycast` | любая поверхность как «расстояние со знаком»; луч по ней |
| `FLUX_ASSERT` | проверка инварианта с хуком (`set_assert_handler`) |
| `Fixed::from_double`, `WorldPos::from_doubles`, `quantize_direction` | **граница с float**: ввод и отображение, симуляция их не вызывает |

## Быстрый старт

```cpp
using namespace Math::literals;
Math::Fixed a = 1.5_fx * Math::Fixed::from_ratio(1, 3);        // 0.49998 (округление к нулю)
Math::Mana cost = 240_mana;                                      // cost + a — не компилируется
Math::FVec3 look = Math::quantize_direction(mouse_x, mouse_y, mouse_z);   // float → Fixed один раз, на границе
```

## Примеры (`examples/`, запускаются как тесты `ctest -L example`)

- `01_fixed_and_world.cpp` — Fixed, насыщение, корень, WorldPos, граница с float, Mana, гравитация.
- `02_determinism_tools.cpp` — Rng, Hasher, шум, `SdfField` и луч, обработчик ассерта.

## Правила

1. В коде симуляции нет `float`, `double`, `std::rand`, системного времени. `from_double` и подобные — только там, где ввод превращается в команды.
2. Порядок обхода, влияющий на результат, не зависит от хеш-таблиц и истории удалений (см. `ECS::World::entities_of`).
3. Сборка с `-Wconversion -Wsign-conversion`: неявные сужения запрещены.

Тесты: `MathTests`; бенчмарк: `MathBenchmarks`.
