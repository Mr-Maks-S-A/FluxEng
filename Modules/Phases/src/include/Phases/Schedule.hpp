#pragma once
/**
 * @file Schedule.hpp
 * @brief Расписание фаз тика: упорядоченный список именованных шагов, который симуляция выполняет по порядку.
 *
 * Порядок фаз — часть правил детерминизма, поэтому он **данные, которые видно**: список имён, а не вызовы,
 * вшитые в один большой `tick`. Новый модуль (машины, гравитация тел, погода) подключается фазой
 * `insert_after("movement", "machines", fn)` и не требует правки чужого кода. Замеры по фазам собираются сами.
 *
 * @code
 * Phases::Schedule schedule;
 * schedule.add("commands", [&] { apply_commands(); })
 *         .add("spells",   [&] { run_spells(); })
 *         .add("terrain",  [&] { apply_edits(); });
 * schedule.insert_after("spells", "machines", [&] { step_machines(); });
 * schedule.run();                       // по порядку: commands, spells, machines, terrain
 * for (const auto& t : schedule.times()) std::println("{} {:.2f} ms", t.name, t.milliseconds);
 * @endcode
 *
 * Время — только для отладки и оверлея, в состояние симуляции оно не попадает.
 */

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Phases {

struct PhaseTime {
    std::string_view name; ///< Живёт, пока фаза в расписании.
    double milliseconds = 0.0;
};

class Schedule {
public:
    using Fn = std::function<void()>;

    Schedule() = default;
    Schedule(const Schedule&) = delete; ///< Замеры хранят string_view на имена внутри расписания.
    Schedule& operator=(const Schedule&) = delete;
    Schedule(Schedule&&) noexcept = default;
    Schedule& operator=(Schedule&&) noexcept = default;

    /// @brief Добавляет фазу в конец. Имена уникальны: повторное имя — нарушение контракта (FLUX_ASSERT).
    Schedule& add(std::string name, Fn fn);
    /// @brief Вставляет фазу перед `anchor` / после него. `false`, если якоря нет (ничего не добавлено).
    bool insert_before(std::string_view anchor, std::string name, Fn fn);
    bool insert_after(std::string_view anchor, std::string name, Fn fn);
    /// @brief Убирает фазу. `false`, если такой нет.
    bool remove(std::string_view name);
    /// @brief Включает/выключает фазу, не убирая из расписания (отладка: «а что, если мана не шагает»). `false` — нет такой.
    bool set_enabled(std::string_view name, bool enabled);

    /// @brief Выполняет включённые фазы по порядку, замеряя каждую.
    void run();

    [[nodiscard]] std::size_t size() const noexcept { return m_phases.size(); }
    [[nodiscard]] bool contains(std::string_view name) const noexcept { return find(name) != npos; }
    /// @brief Имена фаз в порядке выполнения.
    [[nodiscard]] std::vector<std::string_view> names() const;
    /// @brief Время фаз последнего `run()` (в порядке расписания; выключенные — 0).
    [[nodiscard]] std::span<const PhaseTime> times() const noexcept { return m_times; }
    /// @brief Сумма времени фаз последнего `run()`.
    [[nodiscard]] double total_ms() const noexcept { return m_total; }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    struct Phase {
        std::string name;
        Fn fn;
        bool enabled = true;
    };
    [[nodiscard]] std::size_t find(std::string_view name) const noexcept;
    void rebuild_times();

    std::vector<Phase> m_phases;
    std::vector<PhaseTime> m_times;
    double m_total = 0.0;
};

} // namespace Phases
