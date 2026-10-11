/**
 * @file test_input_lockstep.cpp
 * @brief InputSystem + NetSystem: от нажатия клавиши до одной и той же симуляции на всех узлах.
 *
 * Три игрока на разных устройствах и с разными привязками (клавиатура WASD, клавиатура со стрелками, переназначенная из
 * текста настроек, и геймпад) играют через плохую сеть. Путь: события платформы → InputState → ActionMap → InputCommand →
 * NetSystem::Lockstep → симуляция. Проверяется, что
 *  - разные устройства дают одинаковые команды для одного и того же намерения;
 *  - все узлы приходят к побитово одной симуляции на каждом тике, как при воспроизведении без сети;
 *  - запись ввода (InputLog), сделанная «на клавиатуре», воспроизводится в те же команды.
 */

#include <InputSystem/InputSystem.hpp>
#include <NetSystem/NetSystem.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

using namespace InputSystem;
namespace ns = NetSystem;

namespace {

constexpr std::uint32_t kInputDelay = 4;

/// Раскладка команды: одинакова у всех узлов и зашита в игру.
CommandLayout layout() {
    CommandLayout l;
    l.button("cast").axis("move_x").axis("move_y");
    return l;
}

/// Привязки по игрокам. Игрок 2 переназначил управление текстом настроек — код игры тот же.
ActionMap actions_for(ns::PeerId player) {
    if (player == 0) {
        ActionMap a;
        a.bind("cast", Key::Space).bind_keys("move_x", Key::A, Key::D).bind_keys("move_y", Key::W, Key::S);
        return a;
    }
    if (player == 1) {
        ActionMap a;
        a.bind("cast", GamepadButton::X).bind_axis("move_x", GamepadAxis::LeftX, 1.0f, 0.0f).bind_axis("move_y", GamepadAxis::LeftY, 1.0f, 0.0f);
        return a;
    }
    return ActionMap::from_text("cast: Key:J Mouse:Left\nmove_x: Keys:Left,Right\nmove_y: Keys:Up,Down\n").value();
}

/// Сценарий ввода игрока: события по кадрам (кадр = тик, начиная с kInputDelay).
std::vector<InputEvent> script(ns::PeerId player, std::uint32_t frame) {
    const std::uint32_t f = frame - kInputDelay; // кадры до задержки ввода в сети не наблюдаются
    switch (player) {
        case 0: // WASD + пробел
            if (f == 1) return {KeyInput{Key::D, Transition::Press, Modifiers::None}};
            if (f == 3) return {KeyInput{Key::Space, Transition::Press, Modifiers::None}};
            if (f == 4) return {KeyInput{Key::Space, Transition::Release, Modifiers::None}};
            if (f == 6) return {KeyInput{Key::D, Transition::Release, Modifiers::None}, KeyInput{Key::W, Transition::Press, Modifiers::None}};
            if (f == 9) return {KeyInput{Key::W, Transition::Release, Modifiers::None}};
            return {};
        case 1: // геймпад
            if (f == 0) return {GamepadConnectionInput{0, true}};
            if (f == 2) return {GamepadAxisInput{0, GamepadAxis::LeftX, 1.0f}};
            if (f == 3) return {GamepadButtonInput{0, GamepadButton::X, Transition::Press}};
            if (f == 4) return {GamepadButtonInput{0, GamepadButton::X, Transition::Release}};
            if (f == 7) return {GamepadAxisInput{0, GamepadAxis::LeftX, 0.0f}, GamepadAxisInput{0, GamepadAxis::LeftY, -0.5f}};
            return {};
        default: // стрелки + J (переназначено)
            if (f == 1) return {KeyInput{Key::Right, Transition::Press, Modifiers::None}};
            if (f == 3) return {KeyInput{Key::J, Transition::Press, Modifiers::None}};
            if (f == 4) return {KeyInput{Key::J, Transition::Release, Modifiers::None}};
            if (f == 6) return {KeyInput{Key::Right, Transition::Release, Modifiers::None}, KeyInput{Key::Up, Transition::Press, Modifiers::None}};
            if (f == 9) return {KeyInput{Key::Up, Transition::Release, Modifiers::None}};
            return {};
    }
}

/// Устройство игрока: состояние ввода + привязки; даёт команду на тик по сценарию.
struct Device {
    ns::PeerId player = 0;
    ActionMap actions;
    InputState state;
    std::uint32_t next_frame = kInputDelay;
    InputLog log; // запись всего, что пришло «от платформы»

    InputCommand sample(std::uint32_t tick) {
        // Узел снимает команды строго подряд, начиная с kInputDelay.
        REQUIRE(tick == next_frame);
        ++next_frame;
        state.begin_frame();
        for (const InputEvent& e : script(player, tick)) {
            state.apply(e);
            log.record(tick, e);
        }
        return sample_command(actions, state, layout());
    }
};

/// Симуляция: только целые числа; читает только команды.
struct Sim {
    std::int64_t x[3] = {};
    std::int64_t y[3] = {};
    int casts[3] = {};
    InputCommand previous[3] = {};
    std::uint64_t hash = 1469598103934665603ull;
    std::vector<std::uint64_t> history{1469598103934665603ull};

    void step(std::span<const InputCommand> inputs, std::uint32_t tick) {
        for (std::size_t p = 0; p < inputs.size(); ++p) {
            x[p] += inputs[p].axes[0] * 10 / InputCommand::kAxisMax;
            y[p] += inputs[p].axes[1] * 10 / InputCommand::kAxisMax;
            if (button_pressed(previous[p], inputs[p], 0)) ++casts[p];
            previous[p] = inputs[p];
            hash = (hash ^ static_cast<std::uint64_t>(x[p] * 31 + y[p] * 17 + casts[p])) * 1099511628211ull;
        }
        hash = (hash ^ tick) * 1099511628211ull;
        history.push_back(hash);
    }
};

struct World {
    ns::SimulatedNetwork net;
    std::vector<Device> devices;
    std::vector<Sim> sims;
    std::vector<std::unique_ptr<ns::LockstepNode<InputCommand>>> nodes;

    explicit World(ns::LinkConfig link, std::uint64_t seed) : net(seed, link), devices(3), sims(3) {
        std::vector<ns::ITransport*> transports;
        for (int i = 0; i < 3; ++i) transports.push_back(&net.add_endpoint());
        for (int i = 0; i < 3; ++i) {
            devices[static_cast<std::size_t>(i)].player = static_cast<ns::PeerId>(i);
            devices[static_cast<std::size_t>(i)].actions = actions_for(static_cast<ns::PeerId>(i));
            ns::LockstepNodeConfig config;
            config.lockstep.self = static_cast<ns::PeerId>(i);
            config.lockstep.input_delay = kInputDelay;
            config.lockstep.players = {0, 1, 2};
            config.lockstep.neighbors = i == 0 ? std::vector<ns::PeerId>{1, 2} : std::vector<ns::PeerId>{0};
            config.lockstep.relay = i == 0;
            Device* device = &devices[static_cast<std::size_t>(i)];
            Sim* sim = &sims[static_cast<std::size_t>(i)];
            nodes.push_back(std::make_unique<ns::LockstepNode<InputCommand>>(
                *transports[static_cast<std::size_t>(i)], config, [device](ns::PeerId, std::uint32_t tick) { return device->sample(tick); },
                [sim](std::span<const InputCommand> in, std::uint32_t tick) { sim->step(in, tick); }, [sim] { return sim->hash; }));
        }
    }

    bool run(std::uint32_t ticks) {
        for (std::uint64_t now = 0; now <= 120'000'000; now += 1000) {
            net.advance_to(now);
            for (auto& node : nodes) node->update(now);
            if (std::ranges::all_of(nodes, [&](const auto& n) { return n->tick() >= ticks; })) return true;
        }
        return false;
    }
};

/// Эталон без сети: каждый игрок снимает команды сам, симуляция получает их в том же порядке.
std::vector<std::uint64_t> reference(std::uint32_t ticks, Sim& out) {
    std::vector<Device> devices(3);
    for (int i = 0; i < 3; ++i) {
        devices[static_cast<std::size_t>(i)].player = static_cast<ns::PeerId>(i);
        devices[static_cast<std::size_t>(i)].actions = actions_for(static_cast<ns::PeerId>(i));
    }
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        InputCommand commands[3];
        if (tick >= kInputDelay) {
            for (std::size_t p = 0; p < 3; ++p) commands[p] = devices[p].sample(tick);
        }
        out.step(commands, tick);
    }
    return out.history;
}

} // namespace

TEST_SUITE("InputSystem + NetSystem") {

TEST_CASE("cpu: разные устройства и привязки — одни команды на то же намерение") {
    // Игрок 0 (клавиатура WASD) и игрок 2 (стрелки, переназначено текстом) делают одно и то же в одни кадры.
    Device a, c;
    a.player = 0;
    a.actions = actions_for(0);
    c.player = 2;
    c.actions = actions_for(2);
    for (std::uint32_t tick = kInputDelay; tick < kInputDelay + 12; ++tick) {
        CAPTURE(tick);
        CHECK(a.sample(tick) == c.sample(tick));
    }
}

TEST_CASE("cpu: геймпад и клавиатура — стик вправо даёт ту же ось, что и клавиша «вправо»") {
    Device keyboard, pad;
    keyboard.player = 0;
    keyboard.actions = actions_for(0);
    pad.player = 1;
    pad.actions = actions_for(1);
    InputCommand k, p;
    for (std::uint32_t tick = kInputDelay; tick <= kInputDelay + 3; ++tick) { // кадр 1: D нажата; кадр 2: стик на 1.0
        k = keyboard.sample(tick);
        p = pad.sample(tick);
    }
    CHECK(k.axes[0] == InputCommand::kAxisMax); // D → +1 (клавиша держится с кадра 1)
    CHECK(p.axes[0] == InputCommand::kAxisMax); // стик на 1.0 с кадра 2
}

TEST_CASE("cpu: плохая сеть — все узлы повторяют эталон без сети на каждом тике") {
    Sim ref_sim;
    const auto expected = reference(120, ref_sim);
    CHECK(ref_sim.casts[0] == 1); // пробел один раз
    CHECK(ref_sim.casts[1] == 1); // кнопка геймпада один раз
    CHECK(ref_sim.casts[2] == 1); // J один раз
    CHECK(ref_sim.x[0] > 0);
    CHECK(ref_sim.x[0] == ref_sim.x[2]); // клавиатура WASD и стрелки — одно и то же намерение
    CHECK(ref_sim.y[0] == ref_sim.y[2]);

    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        World world({.latency_us = 45'000, .jitter_us = 40'000, .loss = 0.2, .duplicate = 0.1}, seed);
        REQUIRE(world.run(120));
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& history = world.sims[i].history;
            const std::size_t n = std::min(history.size(), expected.size());
            CHECK(std::equal(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(n), expected.begin()));
        }
        for (const auto& node : world.nodes) {
            CHECK(node->session().stats().desyncs == 0);
            CHECK(node->session().stats().input_conflicts == 0);
        }
    }
}

TEST_CASE("cpu: запись ввода воспроизводится в те же команды (реплей)") {
    // «Живая» игра игрока 0 записывает события; затем из записи воссоздаём состояние и снимаем команды заново.
    Device live;
    live.player = 0;
    live.actions = actions_for(0);
    std::vector<InputCommand> live_commands;
    for (std::uint32_t tick = kInputDelay; tick < kInputDelay + 14; ++tick) live_commands.push_back(live.sample(tick));

    const auto restored = InputLog::from_bytes(live.log.to_bytes());
    REQUIRE(restored.has_value());
    InputState state;
    const ActionMap actions = actions_for(0);
    std::vector<InputCommand> replay_commands;
    for (std::uint32_t tick = kInputDelay; tick < kInputDelay + 14; ++tick) {
        state.begin_frame();
        for (const LoggedEvent& e : restored->events_of_frame(tick)) state.apply(e.event);
        replay_commands.push_back(sample_command(actions, state, layout()));
    }
    CHECK(live_commands == replay_commands);
}

} // TEST_SUITE
