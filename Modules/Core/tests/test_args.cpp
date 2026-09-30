#include <Core/App.hpp>

#include <doctest/doctest.h>

#include <array>

namespace {

Core::AppConfig parse(std::initializer_list<const char*> args) {
    std::array<char*, 16> argv{};
    int argc = 0;
    argv[static_cast<std::size_t>(argc++)] = const_cast<char*>("game");
    for (const char* arg : args) {
        argv[static_cast<std::size_t>(argc++)] = const_cast<char*>(arg);
    }
    return Core::parse_args(Core::AppConfig{.title = "Test"}, argc, argv.data());
}

} // namespace

TEST_CASE("parse_args: без аргументов конфигурация не меняется") {
    const Core::AppConfig config = parse({});
    CHECK(config.title == "Test");
    CHECK(config.max_frames == -1);
    CHECK(config.max_ticks == -1);
    CHECK(config.screenshot.empty());
}

TEST_CASE("parse_args: --frames, --ticks, --screenshot") {
    const Core::AppConfig config = parse({"--frames", "120", "--ticks", "300", "--screenshot", "out.png"});
    CHECK(config.max_frames == 120);
    CHECK(config.max_ticks == 300);
    CHECK(config.screenshot == "out.png");
}

TEST_CASE("parse_args: флаг без значения остаётся игре") {
    const Core::AppConfig config = parse({"--frames"});
    CHECK(config.max_frames == -1);
    REQUIRE(config.extra_args.size() == 1);
    CHECK(config.extra_args[0] == "--frames");
}

TEST_CASE("parse_args: неизвестные аргументы — игре, по порядку") {
    const Core::AppConfig config = parse({"--agents", "50000", "--ticks", "10", "--quiet"});
    CHECK(config.max_ticks == 10);
    REQUIRE(config.extra_args.size() == 3);
    CHECK(config.extra_args[0] == "--agents");
    CHECK(config.extra_args[1] == "50000");
    CHECK(config.extra_args[2] == "--quiet");
}
