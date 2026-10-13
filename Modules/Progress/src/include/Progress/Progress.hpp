#pragma once
/**
 * @file Progress.hpp
 * @brief Путь чародея: опыт, ранги и то, что открывает каждый ранг — руны языка и запас личной маны.
 *
 * Язык рун растёт вместе с чародеем: новичку доступны основы (`TARGET`, `CARVE`, арифметика), «Ученик» получает
 * сравнение и переходы, «Знаток» — переменные, «Мастер» — подпрограммы. Заклинание с рунами, которых чародей ещё
 * не открыл, не запускается (`first_locked`). Опыт идёт за сделанное: каждый эффект на мир приносит `effect_xp(r)`.
 *
 * Модуль не знает ни мира, ни сети: только таблица рангов, счётчики и проверка программы. Целые числа и `Fixed` —
 * состояние входит в хеш симуляции (`hash_mastery`), запись и повтор сохраняют путь побитно.
 *
 * @code
 * Progress::RankTable ranks = Progress::RankTable::default_table();
 * Progress::Mastery me;
 * if (auto locked = ranks.first_locked(*program, me.rank)) { ... }          // «руна CALL откроется на ранге 3»
 * Progress::award(ranks, me, Progress::effect_xp(radius));                  // за каждый эффект
 * max_mana = base + ranks.mana_bonus(me.rank);
 * @endcode
 */

#include <Math/Fixed.hpp>
#include <Math/Hash.hpp>
#include <Math/Mana.hpp>
#include <Runes/Program.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Progress {

/// @brief Один ранг: сколько опыта нужно, как зовётся и что даёт (сверх всех предыдущих рангов).
struct Rank {
    std::uint32_t xp_required = 0;       ///< Накопленный опыт, с которого ранг получен. У первого — 0.
    std::string title;                   ///< Название для интерфейса.
    std::vector<Runes::Rune> unlocks;    ///< Руны, открывающиеся этим рангом.
    Math::Mana mana_bonus{};             ///< Прибавка к максимуму личной маны на этом ранге.
};

/// @brief Состояние чародея: накопленный опыт и текущий ранг.
struct Mastery {
    std::uint32_t xp = 0;
    std::uint8_t rank = 0;
    [[nodiscard]] friend constexpr bool operator==(const Mastery&, const Mastery&) noexcept = default;
};

/// @brief Итог начисления опыта.
struct Award {
    int old_rank = 0;
    int new_rank = 0;
    [[nodiscard]] bool ranked_up() const noexcept { return new_rank > old_rank; }
};

/// @brief Таблица рангов: данные, а не код — её можно заменить целиком (другая игра, уровни кампании).
class RankTable {
public:
    /// @brief Таблица по умолчанию: Новичок → Ученик (100) → Знаток (300) → Мастер (700).
    [[nodiscard]] static RankTable default_table();

    /// @brief Таблица из списка рангов. Опыт должен строго возрастать, первый ранг — с нулевого опыта: иначе `FLUX_ASSERT`.
    explicit RankTable(std::vector<Rank> ranks);

    [[nodiscard]] int rank_count() const noexcept { return static_cast<int>(m_ranks.size()); }
    [[nodiscard]] const Rank& at(int rank) const;
    /// @brief Ранг для накопленного опыта.
    [[nodiscard]] int rank_for_xp(std::uint32_t xp) const noexcept;
    /// @brief Опыт, с которого начинается следующий ранг; `nullopt` — ранг последний.
    [[nodiscard]] std::optional<std::uint32_t> xp_for_next(int rank) const noexcept;
    /// @brief С какого ранга доступна руна (0 — с самого начала; руна, не упомянутая в таблице, доступна всегда).
    [[nodiscard]] int rank_of_rune(Runes::Rune rune) const noexcept;
    [[nodiscard]] bool allowed(int rank, Runes::Rune rune) const noexcept { return rank_of_rune(rune) <= rank; }
    /// @brief Первая руна программы, которой чародей ещё не открыл; `nullopt` — программу можно запускать.
    [[nodiscard]] std::optional<Runes::Rune> first_locked(const Runes::Program& program, int rank) const noexcept;
    /// @brief Суммарная прибавка к максимуму маны на этом ранге (с учётом всех предыдущих).
    [[nodiscard]] Math::Mana mana_bonus(int rank) const noexcept;

private:
    std::vector<Rank> m_ranks;
    std::vector<std::uint8_t> m_rune_rank; ///< По номеру руны: с какого ранга доступна.
};

/// @brief Опыт за один эффект на мир (CARVE/RAISE): 1 + r² по целым метрам радиуса, не меньше 1. Крупные эффекты ценнее.
[[nodiscard]] std::uint32_t effect_xp(Math::Fixed radius) noexcept;

/// @brief Начисляет опыт и пересчитывает ранг. Опыт насыщается (не переполняется).
Award award(const RankTable& table, Mastery& mastery, std::uint32_t xp) noexcept;

/// @brief Добавляет состояние в хеш (опыт и ранг).
void hash_mastery(const Mastery& mastery, Math::Hasher& hasher) noexcept;

} // namespace Progress
