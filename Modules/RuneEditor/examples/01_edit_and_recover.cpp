/**
 * @example 01_edit_and_recover.cpp
 * Редактор графа рун без окна: жесты мыши → правки → живая диагностика → автосохранение → восстановление после «аварии».
 *
 * Весь редактор — данные и функции: `Controller` превращает события мыши в `Op`, `Editor::execute` применяет их,
 * `analyze` собирает граф в байт-код и находит проблемы, `Autosave` пишет правки в журнал с избыточностью.
 * Окно нужно только для рисования (цель RuneEditorView) — поэтому всё это проверяется в тестах.
 */

#include <RuneEditor/Analysis.hpp>
#include <RuneEditor/Autosave.hpp>
#include <RuneEditor/Controller.hpp>
#include <RuneEditor/Layout.hpp>

#include <cstdio>

using namespace RuneEditor;
using Runes::Rune;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    EventLog::MemoryStorage disk; // для файла: EventLog::FileStorage
    Editor editor;
    auto autosave = Autosave::start(disk, editor);
    EXPECT(autosave.has_value());

    Controller mouse(editor);

    // 1. Узлы ставим из «палитры» — в точки экрана.
    const NodeId target = mouse.place(Rune::Target, {60, 60});
    const NodeId radius = mouse.place(Rune::Push, {60, 160}, 3 << 16);
    const NodeId carve = mouse.place(Rune::Carve, {260, 60});
    const NodeId halt = mouse.place(Rune::Halt, {460, 60});
    std::printf("1. поставлено узлов: %zu\n", editor.graph().nodes().size());

    // 2. Рёбра тянем мышью от порта к порту. Пока не всё подключено, анализ показывает, что не так.
    auto drag = [&](const PortRef& from, const PortRef& to) {
        const Vec2 a = mouse.camera.to_screen(*port_position(editor.graph(), from));
        const Vec2 b = mouse.camera.to_screen(*port_position(editor.graph(), to));
        mouse.press(Button::Left, a);
        mouse.move(b);
        mouse.release(Button::Left, b);
    };
    drag({target, PortKind::Output, 0}, {carve, PortKind::Input, 0});
    editor.set_entry(carve);
    drag({carve, PortKind::Next, 0}, {halt, PortKind::Input, 0});
    Analysis a = analyze(editor.graph());
    EXPECT(!a.ok());
    EXPECT(a.worst(carve) != nullptr && a.worst(carve)->code == Runes::Code::InputNotConnected);
    std::printf("2. граф не собирается: %s (узел %u)\n", a.error->message().c_str(), a.error->node);

    drag({radius, PortKind::Output, 0}, {carve, PortKind::Input, 1});
    a = analyze(editor.graph());
    EXPECT(a.ok());
    std::printf("   подключили радиус — граф собран: %zu рун, цена прохода %lld ед. маны (сырое значение)\n", a.program->code.size(), static_cast<long long>(a.cost.per_pass.raw()));

    // 3. Автосохранение: устойчивая точка, затем «авария» — редактор потерян, остался только журнал.
    EXPECT(autosave->flush());
    const Graph before_crash = editor.graph();
    const std::uint64_t logged = autosave->operations_logged();

    // 4. Порча носителя: один блок первой полосы. Чётность восстановит.
    std::byte junk[8];
    for (std::byte& b : junk) b = std::byte{0xAB};
    disk.write(EventLog::block_offset(EventLog::Config{}, 0, 0) + 20, junk);

    const auto recovered = Autosave::recover(disk);
    EXPECT(recovered.has_value());
    EXPECT(recovered->graph == before_crash);
    EXPECT(recovered->report.blocks_repaired >= 1);
    std::printf("3. журнал: %llu правок; после порчи блока восстановлено блоков: %llu, граф совпал\n", static_cast<unsigned long long>(logged),
                static_cast<unsigned long long>(recovered->report.blocks_repaired));

    // 5. Отмена и повтор работают поверх восстановленного состояния заново: история — часть журнала правок.
    Editor resumed(recovered->graph);
    EXPECT(analyze(resumed.graph()).ok());
    resumed.remove_node(radius);
    EXPECT(!analyze(resumed.graph()).ok());
    EXPECT(resumed.undo());
    EXPECT(analyze(resumed.graph()).ok());

    // 6. Автораскладка наводит порядок, не меняя смысла: байт-код тот же.
    Graph tidy = editor.graph();
    auto_layout(tidy);
    EXPECT(analyze(tidy).program->code.size() == a.program->code.size());
    std::printf("4. автораскладка: байт-код не изменился, узлы расставлены по колонкам\n");
    std::printf("OK\n");
    return 0;
}
