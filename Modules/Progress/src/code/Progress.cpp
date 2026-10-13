#include <Progress/Progress.hpp>

#include <Math/Assert.hpp>

#include <algorithm>
#include <limits>

namespace Progress {

using Runes::Rune;

RankTable RankTable::default_table() {
    using Math::Mana;
    return RankTable({
        {0, "Новичок", {}, Mana::from_int(0)},
        {100, "Ученик", {Rune::Lt, Rune::Jmp}, Mana::from_int(100)},
        {300, "Знаток", {Rune::Store, Rune::Load}, Mana::from_int(100)},
        {700, "Мастер", {Rune::Call, Rune::Ret}, Mana::from_int(200)},
    });
}

RankTable::RankTable(std::vector<Rank> ranks) : m_ranks(std::move(ranks)) {
    FLUX_ASSERT(!m_ranks.empty() && m_ranks.size() <= 255, "RankTable: от 1 до 255 рангов");
    FLUX_ASSERT(m_ranks.front().xp_required == 0, "RankTable: первый ранг начинается с нулевого опыта");
    for (std::size_t i = 1; i < m_ranks.size(); ++i) {
        FLUX_ASSERT(m_ranks[i].xp_required > m_ranks[i - 1].xp_required, "RankTable: опыт рангов строго возрастает");
    }
    m_rune_rank.assign(static_cast<std::size_t>(Rune::Count), 0);
    // Руна открывается на самом раннем ранге, который её называет; не названная — доступна с начала.
    std::vector<bool> seen(static_cast<std::size_t>(Rune::Count), false);
    for (std::size_t r = 0; r < m_ranks.size(); ++r) {
        for (const Rune rune : m_ranks[r].unlocks) {
            const auto i = static_cast<std::size_t>(rune);
            if (i >= m_rune_rank.size() || seen[i]) continue;
            seen[i] = true;
            m_rune_rank[i] = static_cast<std::uint8_t>(r);
        }
    }
}

const Rank& RankTable::at(int rank) const {
    FLUX_ASSERT(rank >= 0 && rank < rank_count(), "RankTable::at: ранг вне таблицы");
    return m_ranks[static_cast<std::size_t>(rank)];
}

int RankTable::rank_for_xp(std::uint32_t xp) const noexcept {
    int rank = 0;
    for (std::size_t i = 1; i < m_ranks.size(); ++i) {
        if (xp >= m_ranks[i].xp_required) rank = static_cast<int>(i);
    }
    return rank;
}

std::optional<std::uint32_t> RankTable::xp_for_next(int rank) const noexcept {
    if (rank < 0 || rank + 1 >= rank_count()) return std::nullopt;
    return m_ranks[static_cast<std::size_t>(rank) + 1].xp_required;
}

int RankTable::rank_of_rune(Rune rune) const noexcept {
    const auto i = static_cast<std::size_t>(rune);
    return i < m_rune_rank.size() ? m_rune_rank[i] : 0;
}

std::optional<Rune> RankTable::first_locked(const Runes::Program& program, int rank) const noexcept {
    for (const Runes::Instruction& in : program.code) {
        if (!allowed(rank, in.rune)) return in.rune;
    }
    return std::nullopt;
}

Math::Mana RankTable::mana_bonus(int rank) const noexcept {
    Math::Mana total{};
    for (int r = 0; r <= rank && r < rank_count(); ++r) total += m_ranks[static_cast<std::size_t>(r)].mana_bonus;
    return total;
}

std::uint32_t effect_xp(Math::Fixed radius) noexcept {
    const std::int64_t meters = std::max<std::int64_t>(0, radius.raw / Math::Fixed::one_raw);
    return static_cast<std::uint32_t>(1 + meters * meters);
}

Award award(const RankTable& table, Mastery& mastery, std::uint32_t xp) noexcept {
    Award result;
    result.old_rank = mastery.rank;
    const std::uint64_t sum = static_cast<std::uint64_t>(mastery.xp) + xp;
    mastery.xp = static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, std::numeric_limits<std::uint32_t>::max()));
    mastery.rank = static_cast<std::uint8_t>(std::max(mastery.rank, static_cast<std::uint8_t>(table.rank_for_xp(mastery.xp)))); // ранг не падает
    result.new_rank = mastery.rank;
    return result;
}

void hash_mastery(const Mastery& mastery, Math::Hasher& hasher) noexcept {
    hasher.add(mastery.xp);
    hasher.add(mastery.rank);
}

} // namespace Progress
