# Progress — путь чародея

Опыт, ранги и то, что открывает каждый ранг: **руны языка** и запас личной маны. Язык рун растёт вместе с чародеем —
новичку доступны основы, а переменные и подпрограммы нужно заслужить. Так «путь становления» становится структурой, а не словами.

Подключение: `#include <Progress/Progress.hpp>`, цель `engine::Progress`. Зависит от `Math` и `Runes`. Мира, сети и ECS модуль не знает.

```cpp
auto ranks = Progress::RankTable::default_table();      // Новичок → Ученик (100) → Знаток (300) → Мастер (700)
Progress::Mastery me;
if (auto locked = ranks.first_locked(program, me.rank)) { /* «руна STORE откроется на ранге 2» */ }
Progress::award(ranks, me, Progress::effect_xp(radius)); // за каждый эффект: 1 + r²
max_mana = base + ranks.mana_bonus(me.rank);
```

| Ранг | Опыт | Открывает |
|---|---|---|
| 0 Новичок | 0 | основы: `PUSH DUP DROP ADD MUL SUB CASTER AIM TARGET MANA_AT JMP_IF HALT DRAW CARVE RAISE` |
| 1 Ученик | 100 | `LT`, `JMP` (сравнение, безусловный переход) · +100 к максимуму маны |
| 2 Знаток | 300 | `STORE`, `LOAD` (переменные) · +100 |
| 3 Мастер | 700 | `CALL`, `RET` (подпрограммы) · +200 |

Таблица — **данные** (`RankTable(std::vector<Rank>)`), её можно заменить целиком. Состояние (`Mastery`) — два целых, входит в хеш
(`hash_mastery`), поэтому запись и повтор сохраняют путь побитно.

В `SpellSim` включается `Config::use_ranks`: заклинание с закрытой руной не запускается (`locked_casts()`), эффекты приносят
опыт, ранг растит `ManaPool::max`; хеш `mastery` появляется только при включённой прогрессии (прежние эталоны не меняются).

Пример — `examples/01_ranks.cpp`. Тесты: `ProgressTests`.
