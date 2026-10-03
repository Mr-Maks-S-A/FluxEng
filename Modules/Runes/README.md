# Runes — язык рун

Заклинание — **данные**, а не код C++: программа из рун, которую исполняет маленькая стековая машина. Мана — бюджет исполнения:
каждая руна стоит маны, программа останавливается, когда платить нечем (бесконечный цикл останавливается сам).

Подключение: `#include <Runes/Runes.hpp>`, цель `engine::Runes`. Зависит от `Math`, `ECSSystem`, `EventSystem`.

## Три представления, один байт-код

```
.rune (текст) ──parse_program──┐
                               ├─► Program ─► SpellSystem (стековая машина)
.rungraph (граф) ──compile─────┘      ▲
         ▲                            │
         └──────── decompile ─────────┘   (если значения не живут на стеке между операторами)
```

Руны: `PUSH DUP DROP` · `ADD MUL` · `CASTER AIM TARGET` · `MANA_AT` · `JMP_IF HALT` · `DRAW CARVE RAISE`. Стек из 64 Fixed
(вектор — 3 ячейки), ≤ 256 рун в программе и ≤ 256 исполненных за тик.

## Мир и эффекты

Мир модуль не знает. Руны читают его через `SpellHost` (чувства: `position`, `target`, `density`; кошелёк: `draw`,
`take_personal`, `give_personal`), а то, что должно измениться, возвращают **данными** — `Effect{Carve|Raise, …}` в `EffectBuffer`.
Хозяин мира применяет их в своей фазе тика. Поэтому машина рун — чистая функция: её легко записывать, откатывать, гонять параллельно.

```cpp
Runes::ProgramLibrary library;
library.add_text("carve", "TARGET\nPUSH 2\nCARVE\nHALT\n");           // или load_directory(...) для *.rune и *.rungraph
Runes::SpellSystem spells;
ECS::Entity spell = spells.cast(world, mage, library.find("carve"), aim, Runes::ManaSource::Ambient);
Runes::EffectBuffer effects;
spells.tick(world, host, effects);                                    // в порядке id сущностей; эффекты — в буфер
```

Оплата: руна — малая цена, эффект — `k·r³`; из личного запаса вся цена списывается с мага, из окружающей маны платит поле
(`draw`), а с мага берётся в N = 10 раз меньше. Не хватило — `SpellFailed` и остановка на неоплаченной руне.

## Ошибки — коды

`Diagnostic{code, line, node, detail}`: логика и тесты сравнивают `Code` (29 кодов, `code_name` — стабильное имя), `message()`
строит текст по умолчанию, `format()` — «строка 3: …» / «узел 7: …» для отчётов о загрузке файлов.

Примеры: `01_text_spell.cpp` (текст, оплата, эффекты, «убегающий» цикл, трасса), `02_graph_spell.cpp` (граф, файл, обратная сборка, ошибки).
Тесты: `RunesTests`; бенчмарк: цикл машины ≈ 2 мкс на 256 рун.
