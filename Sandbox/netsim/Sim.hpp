#pragma once
/**
 * @file Sim.hpp
 * @brief Детерминированная магическая симуляция для проверки lockstep: маги, заклинания-болты, блуждающие огни.
 *
 * Правила детерминизма (они же — чек-лист для настоящей игры):
 *  - **только целые числа**: позиции в 1/256 клетки, скорости, урон; `isqrt` вместо `sqrt`; никакого float;
 *  - **нет изменяемого генератора случайных чисел**: «случайность» — хеш от (seed, id, tick), чистая функция;
 *  - **порядок — по данным симуляции**: обход плотных массивов ECS; удаление сущностей — после обхода, по списку;
 *  - **параллельные системы** (движение) пишут только в свой элемент и не читают чужие — результат не зависит от числа потоков;
 *  - хеш состояния — по сырым байтам компонентов (структуры из целых без padding, `has_unique_object_representations`).
 */

#include <ECSSystem/ECSSystem.hpp>
#include <JobSystem/JobSystem.hpp>
#include <RuntimeSystem/RuntimeSystem.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace netsim {

/// Команда ввода игрока на один тик. 8 байт, без padding.
struct Cmd {
    std::int8_t move_x = 0; ///< -1, 0, 1.
    std::int8_t move_y = 0;
    std::uint8_t buttons = 0; ///< Бит 0 — колдовать.
    std::uint8_t reserved = 0;
    std::int16_t aim_x = 0; ///< Куда колдовать (вектор от мага).
    std::int16_t aim_y = 0;
};
static_assert(sizeof(Cmd) == 8 && std::has_unique_object_representations_v<Cmd>);

inline constexpr std::int32_t kWorld = 256 * 256;     ///< Размер мира в 1/256 клетки (256×256 клеток).
inline constexpr std::int32_t kMageSpeed = 384;       ///< За тик.
inline constexpr std::int32_t kBoltSpeed = 1536;
inline constexpr std::int32_t kBoltTtl = 60;
inline constexpr std::int32_t kHitRadius = 640;       ///< 2,5 клетки.
inline constexpr std::int32_t kManaMax = 1000;
inline constexpr std::int32_t kCastCost = 100;
inline constexpr std::int32_t kCastCooldown = 6;
inline constexpr std::uint32_t kWispCap = 400;

// Компоненты: поля одного размера, без padding — хеш считается по байтам.
struct MageBody {
    std::int32_t x = 0, y = 0;
    std::int32_t mana = kManaMax;
    std::int32_t hp = 100;
    std::int32_t score = 0;
    std::int32_t cooldown = 0;
    std::int32_t player = 0;
    std::int32_t deaths = 0;
};
struct BoltBody {
    std::int32_t x = 0, y = 0;
    std::int32_t vx = 0, vy = 0;
    std::int32_t ttl = 0;
    std::int32_t owner = 0;
};
struct WispBody {
    std::int32_t x = 0, y = 0;
    std::int32_t vx = 0, vy = 0;
    std::int32_t hp = 1;
    std::uint32_t id = 0;
};
static_assert(std::has_unique_object_representations_v<MageBody> && std::has_unique_object_representations_v<BoltBody> &&
              std::has_unique_object_representations_v<WispBody>);

/// Искусственная порча состояния — чтобы проверить, что рассинхронизация обнаруживается.
struct Corruption {
    std::uint32_t tick = 0;
    bool enabled = false;
};

[[nodiscard]] inline std::uint64_t mix64(std::uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

/// Целочисленный квадратный корень (округление вниз).
[[nodiscard]] inline std::uint32_t isqrt(std::uint64_t value) noexcept {
    std::uint64_t result = 0, bit = 1ull << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::uint32_t>(result);
}

class MagicWorld final : public RuntimeSystem::Module {
public:
    MagicWorld(std::uint64_t seed, int players, Corruption corruption = {}) : m_seed(seed), m_players(players), m_corruption(corruption) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "MagicWorld"; }

    void init(RuntimeSystem::Runtime&) override {
        for (int p = 0; p < m_players; ++p) {
            const ECS::Entity e = m_world.create();
            const std::uint64_t h = mix64(m_seed ^ (0xA5A5ull + static_cast<std::uint64_t>(p)));
            m_world.emplace<MageBody>(e, MageBody{.x = static_cast<std::int32_t>(h % kWorld), .y = static_cast<std::int32_t>((h >> 20) % kWorld), .player = p});
            m_mages.push_back(e);
        }
    }

    /// @brief Команды всех игроков на ближайший тик (порядок — по PeerId). Вызвать перед Runtime::tick().
    void set_inputs(std::span<const Cmd> inputs) noexcept { m_inputs = inputs; }

    void tick(RuntimeSystem::Runtime& runtime) override {
        apply_inputs();
        move_bolts_and_wisps(runtime.jobs());
        resolve_hits();
        spawn_wisps();
        if (m_corruption.enabled && m_tick == m_corruption.tick) {
            // «Баг»: одно значение чуть другое. Очки не «лечатся» сами, поэтому расхождение остаётся до конца прогона.
            auto mages = m_world.pool<MageBody>().components();
            if (!mages.empty()) mages[0].score += 1;
        }
        ++m_tick;
    }

    [[nodiscard]] std::uint32_t tick_count() const noexcept { return m_tick; }
    [[nodiscard]] std::size_t wisps() const noexcept { return m_world.count<WispBody>(); }
    [[nodiscard]] std::size_t bolts() const noexcept { return m_world.count<BoltBody>(); }
    [[nodiscard]] std::int32_t score(int player) const {
        const MageBody* mage = m_world.get<MageBody>(m_mages[static_cast<std::size_t>(player)]);
        return mage != nullptr ? mage->score : 0;
    }

    /// @brief Хеш всего состояния симуляции (по байтам компонентов в порядке плотных массивов).
    [[nodiscard]] std::uint64_t hash() const noexcept {
        std::uint64_t h = 1469598103934665603ull;
        h = fold(h, m_tick);
        h = fold(h, m_next_wisp_id);
        h = fold_array<MageBody>(h);
        h = fold_array<BoltBody>(h);
        h = fold_array<WispBody>(h);
        return h;
    }

    /// @brief Карта w×h символов: `@` маги, `*` болты, `.` огни.
    [[nodiscard]] std::string ascii(int w, int h) const {
        std::string grid(static_cast<std::size_t>((w + 1) * h), ' ');
        for (int row = 0; row < h; ++row) grid[static_cast<std::size_t>(row * (w + 1) + w)] = '\n';
        const auto put = [&](std::int32_t x, std::int32_t y, char c, bool overwrite) {
            const auto col = static_cast<int>(static_cast<std::int64_t>(x) * w / kWorld);
            const auto row = static_cast<int>(static_cast<std::int64_t>(y) * h / kWorld);
            char& cell = grid[static_cast<std::size_t>(row * (w + 1) + col)];
            if (overwrite || cell == ' ') cell = c;
        };
        for (const WispBody& b : components_of<WispBody>()) put(b.x, b.y, '.', false);
        for (const BoltBody& b : components_of<BoltBody>()) put(b.x, b.y, '*', true);
        for (const MageBody& b : components_of<MageBody>()) put(b.x, b.y, static_cast<char>('0' + b.player % 10), true);
        return grid;
    }

private:
    static std::uint64_t fold(std::uint64_t h, std::uint64_t v) noexcept { return (h ^ v) * 1099511628211ull; }

    template<typename T>
    std::span<const T> components_of() const noexcept {
        const auto* pool = m_world.find_pool<T>();
        return pool != nullptr ? std::span<const T>(pool->components()) : std::span<const T>();
    }

    template<typename T>
    std::uint64_t fold_array(std::uint64_t h) const noexcept {
        const auto items = components_of<T>();
        h = fold(h, items.size());
        const auto* words = reinterpret_cast<const std::uint32_t*>(items.data());
        const std::size_t count = items.size() * sizeof(T) / sizeof(std::uint32_t);
        for (std::size_t i = 0; i < count; ++i) h = fold(h, words[i]);
        return h;
    }

    static std::int32_t clamp_world(std::int64_t v) noexcept { return static_cast<std::int32_t>(v < 0 ? 0 : (v >= kWorld ? kWorld - 1 : v)); }

    void apply_inputs() {
        auto mages = m_world.pool<MageBody>().components();
        for (std::size_t i = 0; i < mages.size() && i < m_inputs.size(); ++i) {
            MageBody& mage = mages[i]; // магов не создают и не удаляют после init: индекс в пуле = номер игрока
            const Cmd& cmd = m_inputs[static_cast<std::size_t>(mage.player)];
            mage.x = clamp_world(static_cast<std::int64_t>(mage.x) + cmd.move_x * kMageSpeed);
            mage.y = clamp_world(static_cast<std::int64_t>(mage.y) + cmd.move_y * kMageSpeed);
            mage.mana = std::min(kManaMax, mage.mana + 1);
            if (mage.cooldown > 0) --mage.cooldown;
            const bool wants_cast = (cmd.buttons & 1u) != 0 && (cmd.aim_x != 0 || cmd.aim_y != 0);
            if (wants_cast && mage.cooldown == 0 && mage.mana >= kCastCost) {
                const std::int64_t ax = cmd.aim_x, ay = cmd.aim_y;
                const std::uint32_t len = isqrt(static_cast<std::uint64_t>(ax * ax + ay * ay));
                BoltBody bolt{.x = mage.x, .y = mage.y,
                              .vx = static_cast<std::int32_t>(ax * kBoltSpeed / static_cast<std::int64_t>(len)),
                              .vy = static_cast<std::int32_t>(ay * kBoltSpeed / static_cast<std::int64_t>(len)),
                              .ttl = kBoltTtl, .owner = mage.player};
                m_world.emplace<BoltBody>(m_world.create(), bolt);
                mage.mana -= kCastCost;
                mage.cooldown = kCastCooldown;
            }
        }
    }

    void move_bolts_and_wisps(JobSystem::Scheduler& jobs) {
        auto bolts = m_world.pool<BoltBody>().components();
        JobSystem::parallel_for(jobs, bolts.size(), 256, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                bolts[i].x = clamp_world(static_cast<std::int64_t>(bolts[i].x) + bolts[i].vx);
                bolts[i].y = clamp_world(static_cast<std::int64_t>(bolts[i].y) + bolts[i].vy);
                --bolts[i].ttl;
            }
        });
        auto wisps = m_world.pool<WispBody>().components();
        const std::uint64_t seed = m_seed;
        const std::uint32_t tick = m_tick;
        JobSystem::parallel_for(jobs, wisps.size(), 128, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                WispBody& w = wisps[i];
                if ((tick + w.id) % 24 == 0) { // раз в 24 тика — новое направление: чистая функция от (seed, id, tick)
                    const std::uint64_t h = mix64(seed ^ (static_cast<std::uint64_t>(w.id) << 20) ^ tick);
                    w.vx = static_cast<std::int32_t>(h & 0xF) * 24 - 180;
                    w.vy = static_cast<std::int32_t>((h >> 8) & 0xF) * 24 - 180;
                }
                w.x = clamp_world(static_cast<std::int64_t>(w.x) + w.vx);
                w.y = clamp_world(static_cast<std::int64_t>(w.y) + w.vy);
            }
        });
    }

    static bool near(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by) noexcept {
        const std::int64_t dx = static_cast<std::int64_t>(ax) - bx, dy = static_cast<std::int64_t>(ay) - by;
        return dx * dx + dy * dy <= static_cast<std::int64_t>(kHitRadius) * kHitRadius;
    }

    void resolve_hits() {
        auto& bolt_pool = m_world.pool<BoltBody>();
        auto& wisp_pool = m_world.pool<WispBody>();
        std::vector<ECS::Entity> dead;
        std::vector<bool> wisp_dead(wisp_pool.size(), false);

        // Болты по порядку: каждый поражает первый подходящий огонёк в порядке массива; огонёк — не больше одного болта.
        auto bolts = bolt_pool.components();
        auto wisps = wisp_pool.components();
        auto mages = m_world.pool<MageBody>().components();
        for (std::size_t b = 0; b < bolts.size(); ++b) {
            bool spent = bolts[b].ttl <= 0;
            for (std::size_t w = 0; w < wisps.size() && !spent; ++w) {
                if (wisp_dead[w] || !near(bolts[b].x, bolts[b].y, wisps[w].x, wisps[w].y)) continue;
                wisp_dead[w] = true;
                spent = true;
                ++mages[static_cast<std::size_t>(bolts[b].owner)].score;
            }
            if (spent) dead.push_back(bolt_pool.entities()[b]);
        }
        // Огоньки, долетевшие до мага, ранят его и гаснут.
        for (std::size_t w = 0; w < wisps.size(); ++w) {
            if (wisp_dead[w]) continue;
            for (MageBody& mage : mages) {
                if (!near(wisps[w].x, wisps[w].y, mage.x, mage.y)) continue;
                wisp_dead[w] = true;
                mage.hp -= 10;
                if (mage.hp <= 0) { // «смерть» мага: возрождение в центре
                    mage.hp = 100;
                    mage.x = mage.y = kWorld / 2;
                    ++mage.deaths;
                    --mage.score;
                }
                break;
            }
        }
        for (std::size_t w = 0; w < wisps.size(); ++w) {
            if (wisp_dead[w]) dead.push_back(wisp_pool.entities()[w]);
        }
        for (const ECS::Entity e : dead) m_world.destroy(e); // после всех обходов
    }

    void spawn_wisps() {
        if (m_tick % 4 != 0 || m_world.count<WispBody>() >= kWispCap) return;
        const std::uint64_t h = mix64(m_seed ^ (0x5EEDull << 24) ^ m_next_wisp_id);
        WispBody w{.x = static_cast<std::int32_t>(h % kWorld), .y = static_cast<std::int32_t>((h >> 24) % kWorld), .hp = 1, .id = m_next_wisp_id++};
        m_world.emplace<WispBody>(m_world.create(), w);
    }

    std::uint64_t m_seed;
    int m_players;
    Corruption m_corruption;
    ECS::World m_world;
    std::vector<ECS::Entity> m_mages;
    std::span<const Cmd> m_inputs;
    std::uint32_t m_tick = 0;
    std::uint32_t m_next_wisp_id = 0;
};

/// @brief Ввод бота: чистая функция от (игрок, тик, seed). Игроки бегают по кругам и стреляют в сторону центра.
[[nodiscard]] inline Cmd bot_input(std::uint64_t seed, std::uint16_t player, std::uint32_t tick) noexcept {
    const std::uint64_t h = mix64(seed ^ (static_cast<std::uint64_t>(player) << 40) ^ (tick / 12));
    Cmd c;
    c.move_x = static_cast<std::int8_t>(static_cast<int>(h & 3) - 1);
    c.move_y = static_cast<std::int8_t>(static_cast<int>((h >> 2) & 3) - 1);
    const std::uint64_t g = mix64(seed ^ (static_cast<std::uint64_t>(player) << 32) ^ tick);
    c.buttons = (g & 3) == 0 ? 1 : 0;
    c.aim_x = static_cast<std::int16_t>(static_cast<int>(g >> 8 & 0x3F) - 32);
    c.aim_y = static_cast<std::int16_t>(static_cast<int>(g >> 16 & 0x3F) - 32);
    return c;
}

} // namespace netsim
