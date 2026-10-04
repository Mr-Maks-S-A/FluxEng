# RuneEditor — конспект для изучения и переписывания

`src/include/RuneEditor/{Editor,Analysis,Geometry,Layout,Controller,Autosave,View}.hpp`, `src/code/*.cpp` (~1400 строк). Зависит от `Math`, `Runes`, `EventLog`; рисование (`View`) — отдельная цель `RuneEditorView` (добавляет `RendererSystem`).

## 1. Зачем и главная идея

Модель **визуального редактора заклинаний** (граф рун), которая работает **без окна и GPU** — поэтому вся логика покрыта тестами; рисование (`View`) — тонкий слой сверху.

```
 мышь ──Controller──► Op ──► Editor::execute ──► Graph ──analyze──► байт-код + проблемы по узлам + цена
 тест / повтор / сеть ──────┘        │ observer
                                      └──► Autosave ──► журнал EventLog (снимок + правки)
```
Принципы:
- **Правка — это данные.** `Op` — простая тривиально копируемая структура: то, что делает мышь, можно записать, воспроизвести, отправить по сети и положить в журнал.
- **Редактор не доверяет себе.** `Editor::execute` проверяет каждую правку (цикл, неверная ширина, чужой узел), а не только интерфейс.
- **Нумерация узлов зависит только от содержимого графа** (`Graph::remove` освобождает старший номер) — повтор правок по снимку даёт тот же граф.

## 2. Карта файлов

| Файл | Что |
|---|---|
| `Editor.hpp/.cpp` | `Op`, `OpResult`, `Editor` — единственная точка изменения графа, история, выбор, превью перетаскивания |
| `Analysis.hpp/.cpp` | `analyze(graph, tuning)` → `Analysis{program, source[pc], error, problems[], cost}` |
| `Geometry.hpp/.cpp` | `Vec2`, `Rect`, порты и рёбра, `edge_polyline` (Безье), `pick` (порт → узел → ребро), `bounds_of` |
| `Layout.hpp/.cpp` | `auto_layout` — колонки по ходу управления, значения левее потребителей |
| `Controller.hpp/.cpp` | жесты мыши: перетащить узел, потянуть ребро, поднять конец ребра, рамка выбора, сдвиг и масштаб холста |
| `Autosave.hpp/.cpp` | снимок + правки в журнал `EventLog`; `recover` восстанавливает граф после порчи |
| `View.hpp/.cpp` | отрисовка графа (цвета по семействам рун, свечение исполнения, значки проблем, замок) |

## 3. Как устроено

### 3.1 `Op` и `Editor::execute`
Виды правок: `AddNode`, `RemoveNode`, `MoveNode`, `SetValue` (только PUSH), `SetInput(node, slot, other)`, `SetNext`, `SetBranch`, `SetEntry`, группы `Begin`/`End`, `Undo`, `Redo`. Поля `Op` плоские (`rune`, `slot`, `node`, `other`, `value`, `x`, `y`); `event_name = "rune_editor.op"` — поэтому `Op` пишется в журнал как обычное событие.
`execute`:
1. Служебные (`Begin/End/Undo/Redo`) — обрабатываются сразу. Undo/Redo — **снимки графа** целиком (`m_undo`, `m_redo`), и не работают внутри открытой группы.
2. Для правок графа **сначала проверка, потом снимок, потом изменение**: нельзя `DUP/DROP` (`AddNode`), нельзя `SetEntry` на не-оператор, `SetValue` на не-PUSH, `SetInput` с неверным слотом или несовместимым источником (`can_connect`: ширина числа/вектора, нет цикла), `SetNext/Branch` только на операторы.
3. Успешная правка вызывает **observer** (так работает `Autosave`) и растит `revision()`.
Группы: `begin()/end()` объединяют несколько правок в **один шаг истории** (например, «поднять конец ребра» = «отключить + подключить»). `preview_move/commit_preview/cancel_preview` двигают узел «вживую» без записи в историю и журнал — шумит только итог.
Выбор (`select`, `select_in`, `erase_selection`) — не правка: в историю не идёт.

### 3.2 `analyze`
`compile_mapped` + диагностика: `program` (если граф компилируется), `source[pc]` → узел (подсветка трассы исполнения), `problems` по узлам (ошибки и **предупреждения**: недостижимый оператор, неиспользуемое значение), `cost` (`estimate_cost`). `of(node)`, `worst(node)`, `first_rune_of(node)` — для значков и подсказок. Вызывается **по требованию**: `revision()` подсказывает, когда пересчитывать.

### 3.3 Геометрия и раскладка
Узлы — круглые глифы (`node_radius` 30). Порты: входы слева дугой (`PortKind::Input`, индекс слота), значение/следующий — справа (`Output`, `Next`), ветка — снизу (`Branch`). Рёбра — кривые Безье. `pick(graph, world, zoom)` ищет под курсором **порт → узел → ребро** (порт приоритетнее; радиус попадания `port_hit_radius` 12). `auto_layout(graph, options)` раскладывает колонками (`column`, `row`) по ходу управления; значения — левее потребителей; возвращает число колонок.

### 3.4 `Controller` — жесты
`press/move/release(Button, screen, shift)`, `wheel`, `place(rune, screen)`, `cancel`. Режимы: `Idle`, `DragNodes` (превью → коммит), `Connect` (тянем ребро от порта; `Connecting{from, cursor, valid}` для отрисовки), `Pan`, `Box` (рамка выделения). `ViewCamera{pan, zoom}` переводит экран ↔ холст (`zoom_at` — зум к курсору, `frame` — вписать граф). Все изменения идут через `Editor::execute`.

### 3.5 `Autosave`
- `start(storage, editor, config)`: создаёт журнал `EventLog`, пишет **снимок** графа (`Runes::serialize` текстом, тип `rune_editor.snapshot`), подключает observer → каждая `Op` пишется как событие журнала, затем `flush`.
- `compact()` — пересоздаёт журнал и пишет новый снимок (журнал растёт только правками между компактификациями).
- `recover(storage)`: `read_all(..., repair=true)`; берёт **последний снимок, который удаётся разобрать** (`parse_graph`), затем **повторяет все `Op` после него** в новом `Editor`. Итог: `Recovered{graph, operations, snapshots, report}`. Журнал с избыточностью переживает порчу носителя (см. `EventLog`).
- Счётчик правок и писатель лежат в куче — `Autosave` можно перемещать, наблюдатель не потеряет адрес.

### 3.6 `View`
`View::draw(renderer, font, editor, controller, marks, area, layer)`; `Marks{activity (свечение исполнения по узлам, из трассы), analysis (проблемы и цена), locked (режим записи/повтора: правок нет, рисуется замок)}`. Цвет — по `Family` (Data/Arithmetic/Context/Sense/Control/Effect). `View` создаёт текстуры диска и кольца один раз.

## 4. Как использовать

```cpp
RuneEditor::Editor editor;
auto target = editor.add_node(Runes::Rune::Target, {40, 60});
auto radius = editor.add_node(Runes::Rune::Push, {40, 160}, Math::Fixed::from_int(2).raw);
auto carve  = editor.add_node(Runes::Rune::Carve, {240, 90});
auto halt   = editor.add_node(Runes::Rune::Halt, {420, 90});
editor.connect({target, RuneEditor::PortKind::Output, 0}, {carve, RuneEditor::PortKind::Input, 0});
editor.connect({radius, RuneEditor::PortKind::Output, 0}, {carve, RuneEditor::PortKind::Input, 1});
editor.connect({carve, RuneEditor::PortKind::Next, 0}, {halt, RuneEditor::PortKind::Input, 0});
editor.set_entry(carve);

RuneEditor::Analysis a = RuneEditor::analyze(editor.graph());
if (a.ok()) { /* a.program → ProgramLibrary::add_program, a.cost.per_pass — цена */ }

EventLog::MemoryStorage storage;
auto autosave = RuneEditor::Autosave::start(storage, editor).value();     // дальше каждая правка пишется в журнал
auto recovered = RuneEditor::Autosave::recover(storage).value();          // после сбоя: тот же граф
```
Где это в проекте: `RuneCell2` (редактор в игре, правки идут командой `SetProgram` + блоб; в режиме записи/повтора — замок), `ModuleProof` (демо-граф «вырезать шар» и `View`), пример `01_edit_and_recover.cpp`.

## 5. Что менять осторожно

- **Всё изменение графа — только через `Editor::execute`.** Мимо него не будут работать история, observer, журнал и проверка.
- Добавляя вид правки: `Op::Kind`, ветка проверки и ветка применения в `execute`, `Controller` (если есть жест), **и** `Autosave::recover` (повтор через `execute` подхватит автоматически — если правка детерминирована).
- Нумерация узлов зависит от содержимого графа (`Graph::remove` освобождает старший номер) — не вводи глобальный счётчик: `recover` перестанет давать тот же граф.
- `Op` содержит `float x, y` и **записывается побайтово**: не добавляй указатели/нетривиальные поля и держи структуру инициализированной (`Op{.kind=...}`), иначе в журнал попадёт мусор из padding.
- Undo/Redo — снимки графа целиком: для очень больших графов это память; ограничения истории пока нет.
- `analyze` дорогой относительно кадра на больших графах — вызывай при изменении `revision()`, не каждый кадр.
- `View` — единственное место с зависимостью от рендера; не тяни зависимость в `Editor`/`Controller`.

## 6. Упражнения для переписывания

1. Добавь `Op::Kind::Duplicate` (копия выделенных узлов со связями) — пройди список из §5; как обеспечить, чтобы `recover` дал тот же граф?
2. Ограничь историю undo (кольцо на N снимков) и сделай дельта-историю (хранить обратные `Op`, а не снимки) — что с группами?
3. Сделай подсказку типа соединения: при перетаскивании ребра подсвечивать порты, к которым `can_connect` вернёт `true`.
4. Реализуй автокомплит рун по ширине входа (`Runes::input_widths`) при отпускании ребра в пустом месте.
5. Добавь копирование/вставку графа через буфер обмена в виде `serialize` (`.rungraph`) и проверку через `parse_graph`.

## 7. Тесты и примеры

`Modules/RuneEditor/tests` (5 файлов, 26 тестов: правки и отклонение недопустимых, история и группы, анализ, геометрия/выбор, жесты, автосохранение и восстановление после порчи), пример `01_edit_and_recover.cpp` (весь цикл от жестов до восстановления после порчи носителя).
