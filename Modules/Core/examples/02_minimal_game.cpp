/**
 * @example 02_minimal_game.cpp
 * Минимальная игра на Core: окно, фиксированный тик 60 Гц, события шины и ввод через действия — всё в одном файле.
 *
 * Игра — это наследник `Core::Game` и одна строка в `main()`. `Core::App` владеет окном, рендером и шиной и крутит цикл:
 * кадр (ввод, камера, рисование) отдельно от тика симуляции (фиксированная частота). Здесь окно скрыто, цикл
 * заканчивается через 120 тиков, а «нажатие» клавиши вводится программно.
 */

#include <Core/Core.hpp>

#include <GLFW/glfw3.h>

#include <cstdio>

namespace es = EventSystem;

namespace {

/// Событие игры: прыжок. События — тривиальные структуры с именем и описанием полей.
struct JumpEvent {
    std::int32_t at_tick = 0;
    static constexpr std::string_view event_name = "demo.jump";
    using fields = es::Fields<es::Field<"at_tick", &JumpEvent::at_tick>>;
};

struct Counters {
    int setups = 0, frames = 0, ticks = 0, jumps_sent = 0, jumps_seen = 0;
    bool shutdown = false;
} g;

class Demo final : public Core::Game {
public:
    // 1. setup: объявить модули и события, получить писателей и читателей, привязать действия.
    void setup(Core::App& app) override {
        ++g.setups;
        const es::ModuleId id = app.bus().declare_module("Demo").produces<JumpEvent>().consumes<JumpEvent>();
        jumps_out = app.bus().writer<JumpEvent>(id);
        jumps_in = app.bus().reader<JumpEvent>(id);
        jump = actions.declare("jump", "Прыжок");
        actions.bind(jump, Core::Binding::key(GLFW_KEY_SPACE));
    }

    // 2. frame: начало кадра — ввод (в домене кадра), обзор камерой, загрузка данных на GPU. Тиков здесь ещё нет.
    void frame(Core::App& app, float /*seconds*/) override {
        ++g.frames;
        if (g.frames == 30) { // «игрок» нажимает пробел: программное нажатие через окно (событие доходит до ввода сразу)
            app.window().inject_key(GLFW_KEY_SPACE, GLFW_PRESS);
            app.window().inject_key(GLFW_KEY_SPACE, GLFW_RELEASE);
        }
        // «Нажато в этом кадре» живёт один кадр, а тиков за кадр может быть 0 или несколько — поэтому запоминаем намерение.
        if (actions.pressed(app.window().input(), jump)) want_jump = true;
    }

    // 3. tick: один шаг симуляции (фиксированная частота). Читаем события прошлого тика, пишем события этого.
    void tick(Core::App& app) override {
        ++g.ticks;
        g.jumps_seen += static_cast<int>(jumps_in.events().size());
        if (want_jump) {
            want_jump = false;
            ++g.jumps_sent;
            (void)jumps_out.emit(JumpEvent{static_cast<std::int32_t>(app.tick())});
        }
    }

    // 4. Рисование: 3D-сцена, 2D-мир и оверлей — отдельные хуки в порядке кадра. Здесь рисовать нечего.
    void render_overlay(Core::App&, RendererSystem::Renderer2D&) override {}

    void shutdown(Core::App&) override { g.shutdown = true; }

private:
    Core::ActionMap actions;
    Core::ActionId jump;
    es::EventWriter<JumpEvent> jumps_out;
    es::EventReader<JumpEvent> jumps_in;
    bool want_jump = false;
};

} // namespace

int main(int argc, char** argv) {
    // max_ticks включает «lockstep»: ровно один тик на кадр — прогон детерминирован и не зависит от скорости машины.
    // pause_key = 0: по умолчанию пауза висит на пробеле, а пример сам «нажимает» пробел — иначе тики встали бы навсегда.
    const int code = Core::run<Demo>({.title = "MinimalGame", .width = 320, .height = 240, .visible = false, .ticks_per_second = 60.0, .max_ticks = 120, .pause_key = 0},
                                     argc, argv);
    std::printf("\nsetup %d, кадров %d, тиков %d, прыжков отправлено %d / получено %d, shutdown вызван: %s\n", g.setups, g.frames, g.ticks, g.jumps_sent, g.jumps_seen,
                g.shutdown ? "да" : "нет");
    const bool ok = code == 0 && g.setups == 1 && g.ticks == 120 && g.jumps_sent == 1 && g.jumps_seen == 1 && g.shutdown;
    std::printf("%s\n", ok ? "OK" : "ОШИБКА");
    return ok ? 0 : 1;
}
