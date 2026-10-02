#pragma once
/**
 * @file Tags.hpp
 * @brief Теги памяти (как MEMORY_TAG_* в Kohi): сколько физической памяти держит каждая подсистема.
 *
 * Каждая арена и пул получают тег при создании (`Arena::reserve(..., MemoryTag::Events)`).
 * Учитывается **подтверждённая** память (commit) — то, что реально занимает ОЗУ, — и пик по тегу.
 * Счётчики атомарные: арены потоков JobSystem растут параллельно.
 *
 * @code
 * auto scratch = MemorySystem::Arena::reserve(MiB(64), KiB(64), MemorySystem::MemoryTag::Jobs);
 * ...
 * std::print("{}", MemorySystem::memory_report()); // таблица по тегам: сейчас, пик, арен
 * @endcode
 *
 * Это набросок: стандартные контейнеры (`std::vector` в ECS, шине) сюда пока не попадают —
 * для них нужен тегированный std::pmr-ресурс или свой аллокатор (см. конспект 8).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace MemorySystem {

/// @brief Кому принадлежит память.
enum class MemoryTag : std::uint8_t {
    Untagged,  ///< Тег не задан — такие арены и стоит искать первыми.
    Engine,    ///< Ядро приложения (Core: память тика, кадра).
    Events,    ///< Шина событий.
    ECS,       ///< Мир сущностей.
    Jobs,      ///< Временные арены потоков JobSystem.
    Renderer,  ///< Рендер (CPU-сторона: батчи, атласы).
    Game,      ///< Данные игры.
    Scratch,   ///< Короткоживущая временная память.
    Count,
};

/// @brief Имя тега для отчёта.
[[nodiscard]] std::string_view to_string(MemoryTag tag) noexcept;

/// @brief Счётчики одного тега.
struct TagStats {
    std::size_t committed = 0; ///< Подтверждено сейчас, байт.
    std::size_t peak = 0;      ///< Максимум committed.
    std::size_t regions = 0;   ///< Живых арен/пулов с этим тегом.
};

/// @brief Счётчики тега (снимок).
[[nodiscard]] TagStats tag_stats(MemoryTag tag) noexcept;

/// @brief Таблица «тег — сейчас — пик — арен» (пустые теги пропускаются).
[[nodiscard]] std::string memory_report();

namespace detail {
void track_commit(MemoryTag tag, std::size_t bytes) noexcept;
void track_decommit(MemoryTag tag, std::size_t bytes) noexcept;
void track_region(MemoryTag tag, int delta) noexcept;
} // namespace detail

} // namespace MemorySystem
