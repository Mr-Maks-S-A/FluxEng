#pragma once
/**
 * @file Core.hpp
 * @brief Общий заголовок модуля Core и точка входа игры.
 *
 * @code
 * int main(int argc, char** argv) {
 *     return Core::run<MyGame>({.title = "MyGame", .ticks_per_second = 30.0}, argc, argv);
 * }
 * @endcode
 */

#include <Core/App.hpp>
#include <Core/FixedStep.hpp>
#include <Core/PlatformEvents.hpp>

#include <concepts>
#include <exception>
#include <print>
#include <string>
#include <utility>

namespace Core {

/**
 * @brief Создаёт App и игру `G`, запускает цикл и возвращает код выхода процесса.
 *
 * Аргументы командной строки (`--frames`, `--ticks`, `--screenshot`) дополняют `config`.
 * Исключение при запуске (нет окна, ошибка шейдера, нарушение контракта шины)
 * печатается в stderr, а код выхода становится 1.
 */
template<std::derived_from<Game> G>
int run(AppConfig config, int argc, char** argv) {
    const std::string title = config.title;
    try {
        App app(parse_args(std::move(config), argc, argv));
        G game;
        return app.run(game);
    } catch (const std::exception& error) {
        std::println(stderr, "{}: {}", title, error.what());
        return 1;
    }
}

} // namespace Core
