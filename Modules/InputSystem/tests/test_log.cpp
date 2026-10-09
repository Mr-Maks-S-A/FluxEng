#include <InputSystem/InputSystem.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace InputSystem;

namespace {

std::uint32_t crc32(const std::byte* data, std::size_t size) {
    std::uint32_t crc = ~0u;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= std::to_integer<std::uint32_t>(data[i]);
        for (int k = 0; k < 8; ++k) crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return ~crc;
}

/// Пересчитать CRC в конце записи после ручной правки байтов — чтобы проверить разбор данных, а не контрольную сумму.
void fix_crc(std::vector<std::byte>& bytes) {
    const std::uint32_t crc = crc32(bytes.data(), bytes.size() - 4);
    std::memcpy(bytes.data() + bytes.size() - 4, &crc, 4);
}

/// Запись со всеми видами событий.
InputLog sample_log() {
    InputLog log;
    log.record(0, CursorInput{10.5, 20.25});
    log.record(1, KeyInput{Key::W, Transition::Press, Modifiers::Shift});
    log.record(1, MouseButtonInput{MouseButton::Right, Transition::Press, Modifiers::None});
    log.record(3, KeyInput{Key::W, Transition::Release, Modifiers::None});
    log.record(3, ScrollInput{0.0, -1.0});
    log.record(3, CharInput{0x43F});
    log.record(5, FocusInput{false});
    log.record(5, FocusInput{true});
    log.record(6, GamepadConnectionInput{1, true});
    log.record(6, GamepadButtonInput{1, GamepadButton::Start, Transition::Press});
    log.record(7, GamepadAxisInput{1, GamepadAxis::LeftTrigger, 0.625f});
    return log;
}

} // namespace

TEST_SUITE("InputSystem.Log") {

TEST_CASE("запись по кадрам: порядок, выборка кадра, откат назад запрещён") {
    InputLog log = sample_log();
    CHECK(log.size() == 11);
    CHECK(log.last_frame() == 7);
    CHECK(log.events_of_frame(0).size() == 1);
    CHECK(log.events_of_frame(1).size() == 2);
    CHECK(log.events_of_frame(2).empty()); // кадр без событий
    CHECK(log.events_of_frame(3).size() == 3);
    CHECK(log.events_of_frame(99).empty());
    CHECK_FALSE(log.record(2, FocusInput{true})); // кадр раньше последнего
    CHECK(log.size() == 11);
    CHECK(std::holds_alternative<KeyInput>(log.events_of_frame(1)[0].event));
    CHECK(std::holds_alternative<MouseButtonInput>(log.events_of_frame(1)[1].event));
}

TEST_CASE("двоичный круг: все виды событий возвращаются без потерь, формат стабилен") {
    const InputLog log = sample_log();
    const auto bytes = log.to_bytes();
    const auto parsed = InputLog::from_bytes(bytes);
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error());
    CHECK(*parsed == log);
    CHECK(parsed->to_bytes() == bytes);
    CHECK(std::memcmp(bytes.data(), "FXIL", 4) == 0);
    CHECK(InputLog{}.to_bytes().size() == 16); // заголовок + CRC
    CHECK(InputLog::from_bytes(InputLog{}.to_bytes())->empty());
}

TEST_CASE("воспроизведение даёт то же состояние, что и живой ввод, на каждом кадре") {
    // «Живая» игра: события приходят по кадрам, состояние пишется в журнал.
    InputLog log;
    InputState live;
    std::vector<std::array<bool, 4>> seen;
    const std::array<std::pair<Key, std::array<Transition, 6>>, 2> script = {{
        {Key::Space, {Transition::Release, Transition::Press, Transition::Release, Transition::Release, Transition::Release, Transition::Release}},
        {Key::D, {Transition::Release, Transition::Release, Transition::Press, Transition::Repeat, Transition::Release, Transition::Release}},
    }};
    for (std::uint32_t frame = 0; frame < 6; ++frame) {
        live.begin_frame();
        for (const auto& [key, transitions] : script) {
            if (transitions[frame] == Transition::Release && !live.down(key)) continue; // отпускать нечего
            const KeyInput e{key, transitions[frame], Modifiers::None};
            live.apply(e);
            log.record(frame, e);
        }
        seen.push_back({live.down(Key::Space), live.pressed(Key::Space), live.down(Key::D), live.released(Key::D)});
    }
    const auto replay = InputLog::from_bytes(log.to_bytes());
    REQUIRE(replay.has_value());
    InputState replayed;
    for (std::uint32_t frame = 0; frame < 6; ++frame) {
        replayed.begin_frame();
        for (const LoggedEvent& e : replay->events_of_frame(frame)) replayed.apply(e.event);
        const std::array<bool, 4> now = {replayed.down(Key::Space), replayed.pressed(Key::Space), replayed.down(Key::D), replayed.released(Key::D)};
        CAPTURE(frame);
        CHECK(now == seen[frame]);
    }
}

TEST_CASE("любое усечение записи — ошибка") {
    const auto bytes = sample_log().to_bytes();
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        CAPTURE(n);
        CHECK_FALSE(InputLog::from_bytes(std::span<const std::byte>(bytes.data(), n)).has_value());
    }
}

TEST_CASE("порча любого байта ловится контрольной суммой") {
    const auto bytes = sample_log().to_bytes();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        CAPTURE(i);
        auto bad = bytes;
        bad[i] ^= std::byte{0x21};
        CHECK_FALSE(InputLog::from_bytes(bad).has_value());
    }
}

TEST_CASE("данные с верной суммой, но неверными значениями отвергаются") {
    const auto good = [] {
        InputLog log;
        log.record(0, KeyInput{Key::A, Transition::Press, Modifiers::None});
        return log.to_bytes();
    }();
    // Вход: заголовок 12 байт, затем кадр u32, тип u8, key u16, transition u8, mods u8.
    const auto patched = [&](std::size_t offset, std::uint8_t value) {
        auto bytes = good;
        bytes[offset] = static_cast<std::byte>(value);
        fix_crc(bytes);
        return InputLog::from_bytes(bytes);
    };
    CHECK(patched(12 + 4 + 1 + 2, 1).has_value());    // transition = Press: исходное значение, для контроля
    CHECK_FALSE(patched(12 + 4 + 1 + 2, 7).has_value()); // transition вне диапазона
    CHECK_FALSE(patched(12 + 4 + 1 + 3, 200).has_value()); // mods вне маски
    CHECK_FALSE(patched(12 + 4, 99).has_value());      // неизвестный тип события
    CHECK_FALSE(patched(4, 9).has_value());            // версия 9

    // Курсор: NaN вместо числа.
    InputLog cursor;
    cursor.record(0, CursorInput{1.0, 2.0});
    auto bytes = cursor.to_bytes();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::memcpy(bytes.data() + 12 + 4 + 1, &nan, sizeof(nan));
    fix_crc(bytes);
    CHECK_FALSE(InputLog::from_bytes(bytes).has_value());
}

TEST_CASE("число событий в заголовке не заставляет выделять память: правда выясняется при чтении") {
    auto bytes = InputLog{}.to_bytes();
    const std::uint32_t huge = 0x00FFFFFF;
    std::memcpy(bytes.data() + 8, &huge, 4);
    fix_crc(bytes);
    const auto r = InputLog::from_bytes(bytes);
    REQUIRE_FALSE(r.has_value());
    const std::uint32_t absurd = 0xFFFFFFFF;
    std::memcpy(bytes.data() + 8, &absurd, 4);
    fix_crc(bytes);
    CHECK_FALSE(InputLog::from_bytes(bytes).has_value());
}

TEST_CASE("кадры не могут идти назад даже в испорченном файле") {
    InputLog log;
    log.record(5, FocusInput{true});
    log.record(6, FocusInput{true});
    auto bytes = log.to_bytes();
    const std::uint32_t earlier = 2;
    std::memcpy(bytes.data() + 12 + 4 + 1 + 1, &earlier, 4); // кадр второго события: после u32+u8+u8 первого
    fix_crc(bytes);
    CHECK_FALSE(InputLog::from_bytes(bytes).has_value());
}

TEST_CASE("случайная порча при исправленной сумме: разбор всегда завершается штатно") {
    const auto good = sample_log().to_bytes();
    std::mt19937 rng(2024);
    for (int round = 0; round < 3000; ++round) {
        auto bytes = good;
        const int flips = 1 + static_cast<int>(rng() % 3);
        for (int f = 0; f < flips; ++f) bytes[12 + rng() % (bytes.size() - 16)] = static_cast<std::byte>(rng());
        fix_crc(bytes);
        (void)InputLog::from_bytes(bytes); // результат любой; главное — без падений и без UB
    }
}

} // TEST_SUITE
