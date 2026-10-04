#pragma once
/**
 * @file Module.hpp
 * @brief Интерфейс модуля игры: жизненный цикл declare → init → (frame, tick)* → shutdown и зависимости по именам.
 *
 * Модуль — единица игровой логики или сервиса (физика, мир вокселей, сеть, заклинания). Он не знает ни об окне,
 * ни о GPU: всё, что ему нужно, он берёт из Runtime (шина, задачи, память тика). Поэтому один и тот же модуль
 * работает и в клиенте, и на выделенном сервере.
 *
 * ```
 * Runtime::initialize():   порядок по зависимостям → declare() всех → init() каждого
 * каждый кадр:             frame() всех             (ввод, интерполяция; идёт и на паузе)
 * каждый тик:              tick() всех              → обратный вызов игры → шина advance_tick → смена арен
 * Runtime::shutdown():     shutdown() каждого в ОБРАТНОМ порядке init
 * ```
 *
 * Гарантии:
 * - `shutdown()` вызывается только у модулей, чей `init()` завершился успешно; частично инициализированный
 *   модуль убирает за собой сам (бросил `init()` — значит, ресурсов не осталось);
 * - `shutdown()` не бросает (`noexcept` — часть сигнатуры) и вызывается ровно один раз, даже при исключении
 *   в тике и даже если Runtime просто уничтожен;
 * - зависимость всегда инициализируется раньше зависимого и завершается позже него.
 */

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RuntimeSystem {

class Runtime;

/// @brief Базовый класс модуля. Наследник задаёт имя, объявляет зависимости в конструкторе и переопределяет нужные хуки.
class Module {
public:
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    virtual ~Module() = default;

    /// @brief Уникальное имя модуля: по нему находятся зависимости и печатается порядок.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// @brief Имена модулей, которые должны быть инициализированы раньше этого.
    [[nodiscard]] std::span<const std::string> dependencies() const noexcept { return m_dependencies; }

    /// @brief Объявить события (`runtime.bus().declare_module(name()).produces<…>()`). Все declare() идут до первого init():
    /// к моменту init() контракты всех модулей уже известны, и можно брать писателей и читателей.
    virtual void declare(Runtime& /*runtime*/) {}

    /// @brief Создать ресурсы и взять писателей/читателей шины. Зависимости уже инициализированы.
    virtual void init(Runtime& /*runtime*/) {}

    /// @brief Начало кадра: ввод и интерполяция в домене кадра. `seconds` — длительность прошлого кадра. Идёт и на паузе.
    virtual void frame(Runtime& /*runtime*/, float /*seconds*/) {}

    /// @brief Один тик симуляции. Модули вызываются в порядке инициализации (зависимости — раньше зависимых).
    virtual void tick(Runtime& /*runtime*/) {}

    /// @brief Освободить ресурсы. Не бросает. Зависимости ещё живы (они завершаются позже).
    virtual void shutdown(Runtime& /*runtime*/) noexcept {}

protected:
    /// @brief Объявить зависимость по имени модуля. Вызывать в конструкторе наследника.
    void depends_on(std::string_view module_name) { m_dependencies.emplace_back(module_name); }

private:
    std::vector<std::string> m_dependencies;
};

} // namespace RuntimeSystem
