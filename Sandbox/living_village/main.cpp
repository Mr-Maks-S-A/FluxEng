/**
 * @file main.cpp
 * @brief LivingVillage — вертикальный срез «Живая деревня»: игрок — Искра, первая живая мана; жители деревни живут с маной по-разному.
 *
 * Это прототип замысла, а не демонстрация движка: «мир только начинает жить, жители уже есть, а игрок — мана, которая течёт
 * по миру, и от того, как она течёт, зависит, кем станут эти люди». Без злодеев и фракций-ярлыков: восемь жителей четырёх укладов,
 * и каждый по-своему читает Искру:
 *   - **добывающие** (Extractor) берут ману из поля в запас и отдают десятину чтущим;
 *   - **чтущие** (Reverent) вносят дары в поле из запаса и тянутся к той Искре, которой верят;
 *   - **пугливые** (Fearful) бегут от резких всплесков маны рядом с Искрой;
 *   - **не замечающие** (Oblivious) молча живут, пока место не обеднеет, а потом переселяются туда, где мана богаче.
 * Круговорот: поле → добывающие → (десятина) → чтущие → (дары) → поле. Искра встраивается в него: дарит и забирает ману,
 * и деревня меняется — видимо и быстро, без единой строчки «сюжета».
 *
 * Всё детерминированно (Fixed и целые, порядок жителей по id, `Math::Rng` с сидом): прогон записывается и воспроизводится
 * (`Replay`), хеши подсистем (`mana`, `villagers`, `spark`, `tick`) сходятся. Консольный: окна и GPU нет.
 *
 * Использование:
 *   LivingVillage [--seed N] [--ticks T] [--policy generous|greedy|silent|wander] [--chronicle] [--record f | --replay f]
 *   LivingVillage --check        самопроверка (детерминизм, запись и повтор, «деревня зависит от Искры»); последняя строка `OK`
 */

#include <ECSSystem/ECSSystem.hpp>
#include <ManaField/ManaField.hpp>
#include <Math/Math.hpp>
#include <Phases/Schedule.hpp>
#include <Replay/Replay.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using Math::Fixed;
using Math::FVec3;
using Math::Mana;
using Math::WorldPos;

namespace {

// ------------------------------------------------------------------------------------------------ мир и константы

constexpr std::int64_t meter = Fixed::one_raw;
constexpr std::int64_t world_size = 64 * meter;      ///< Деревня живёт на площадке 64×64 м (плоскость y = 0).
constexpr int tick_hz = 10;                          ///< 1 тик = 0,1 с; поле маны шагает каждый тик.
constexpr int villager_count = 8;
const Mana extractor_stock_cap = Mana::from_int(150);

enum class Way : std::uint8_t { Extractor, Reverent, Fearful, Oblivious };

const char* way_name(Way way) {
    switch (way) {
    case Way::Extractor: return "добывающий";
    case Way::Reverent: return "чтящий";
    case Way::Fearful: return "пугливый";
    case Way::Oblivious: return "незамечающий";
    }
    return "?";
}

enum class Cmd : std::uint16_t { Move = 1, Give = 2, Take = 3 };

Replay::Command move_command(Fixed dx, Fixed dz) { return {.type = static_cast<std::uint16_t>(Cmd::Move), .arg = 0, .x = dx.raw, .y = 0, .z = dz.raw}; }
Replay::Command give_command(int amount) { return {.type = static_cast<std::uint16_t>(Cmd::Give), .arg = static_cast<std::int16_t>(amount)}; }
Replay::Command take_command(int amount) { return {.type = static_cast<std::uint16_t>(Cmd::Take), .arg = static_cast<std::int16_t>(amount)}; }

void register_commands(Replay::CommandRegistry& registry) {
    using Field = Replay::CommandField;
    registry.add({static_cast<std::uint16_t>(Cmd::Move), "move", {Field{}, Field{"dx", Field::Kind::Fixed}, Field{}, Field{"dz", Field::Kind::Fixed}}})
        .add({static_cast<std::uint16_t>(Cmd::Give), "give", {Field{"amount", Field::Kind::Int}, Field{}, Field{}, Field{}}})
        .add({static_cast<std::uint16_t>(Cmd::Take), "take", {Field{"amount", Field::Kind::Int}, Field{}, Field{}, Field{}}});
}

// ------------------------------------------------------------------------------------------------ компоненты

/// Житель. Только целые и Fixed: хешируется побайтово по полям, не зависит от платформы.
struct Villager {
    Way way = Way::Oblivious;
    std::uint8_t flags = 0;       ///< bit0 — бежит, bit1 — переселяется (чтобы записывать в летопись только смену состояния).
    std::uint16_t name = 0;       ///< Код имени (два слога).
    Fixed vitality{};             ///< 0…1: насколько жителю хорошо там, где он стоит (плотность маны вокруг).
    Fixed trust{};                ///< −1…1: что он думает об Искре.
    Mana stock{};                 ///< Запас маны (у добывающих — добытое; у чтущих — для даров).
    WorldPos pos{};
    WorldPos home{};
    std::uint32_t unease = 0;     ///< Сколько тиков подряд жителю плохо; слишком долго — он уходит.
};

enum class EventKind : std::uint8_t { Offering, Tithe, Fled, Migrated, Pilgrim, Departed };

struct Event {
    std::uint32_t tick = 0;
    EventKind kind = EventKind::Offering;
    std::uint16_t who = 0;     ///< Код имени.
    std::uint16_t other = 0;   ///< Код имени второго участника (десятина) или 0.
};

const char* const syllables[] = {"Ар", "Бе", "Ви", "Ге", "Да", "Ел", "Жи", "Зо", "Ин", "Ка", "Ло", "Ми", "На", "Ор", "Пи", "Ру"};
std::string name_of(std::uint16_t code) {
    if (code == 0) return "—";
    return std::string(syllables[code & 15]) + syllables[(code >> 4) & 15];
}

// ------------------------------------------------------------------------------------------------ симуляция

class Village {
public:
    explicit Village(std::uint64_t seed) : m_rng(seed), m_mana(ManaField::Config{}) {
        const WorldPos centre{world_size / 2, 0, world_size / 2};
        m_spark.pos = centre;
        m_spark.reserve = Mana::from_int(600);
        for (int i = 0; i < villager_count; ++i) {
            const ECS::Entity e = m_world.create();
            Villager v;
            v.way = static_cast<Way>(i % 4);
            v.name = static_cast<std::uint16_t>(1 + m_rng.below(255));
            v.vitality = Fixed::from_int(1);
            v.pos = {centre.x + (static_cast<std::int64_t>(m_rng.below(13)) - 6) * meter, 0, centre.z + (static_cast<std::int64_t>(m_rng.below(13)) - 6) * meter};
            v.home = v.pos;
            v.stock = v.way == Way::Reverent ? Mana::from_int(60) : Mana{};
            m_world.emplace<Villager>(e, v);
        }
        m_schedule.add("commands", [this] { phase_commands(); })
            .add("spark", [this] { phase_spark(); })
            .add("villagers", [this] { phase_villagers(); })
            .add("mana", [this] { m_mana.step(); })
            .add("aftermath", [this] { phase_aftermath(); });
    }

    void tick(std::span<const Replay::Command> commands) {
        m_commands = commands;
        m_schedule.run();
        m_commands = {};
        ++m_tick;
    }
    [[nodiscard]] std::uint32_t tick_number() const noexcept { return m_tick; }

    [[nodiscard]] Replay::StateHashes hashes() const {
        Math::Hasher villagers, spark, tick;
        for (const ECS::Entity e : m_world.entities_of<Villager>()) {
            const Villager& v = *m_world.get<Villager>(e);
            villagers.add(e.index);
            villagers.add(static_cast<std::uint64_t>(v.way)), villagers.add(v.flags), villagers.add(v.name);
            villagers.add_signed(v.vitality.raw), villagers.add_signed(v.trust.raw), villagers.add_signed(v.stock.raw());
            villagers.add_signed(v.pos.x), villagers.add_signed(v.pos.z), villagers.add(v.unease);
        }
        spark.add_signed(m_spark.pos.x), spark.add_signed(m_spark.pos.z), spark.add_signed(m_spark.reserve.raw()), spark.add_signed(m_spark.activity.raw);
        tick.add(m_tick);
        Replay::StateHashes h;
        h.add("mana", m_mana.hash()).add("villagers", villagers.value()).add("spark", spark.value()).add("tick", tick.value());
        return h;
    }

    // --- состояние наружу (только чтение): для отчёта, политик Искры и проверок
    struct Spark {
        WorldPos pos{};
        FVec3 wish{};
        Mana reserve{};
        Fixed activity{};          ///< Недавние дары и взятия (затухает): пугливые боятся именно резкости.
        Mana gave{}, took{};       ///< За этот тик.
    };
    [[nodiscard]] const Spark& spark() const noexcept { return m_spark; }
    [[nodiscard]] const ECS::World& world() const noexcept { return m_world; }
    [[nodiscard]] const ManaField::ManaGrid& mana() const noexcept { return m_mana; }
    [[nodiscard]] const std::vector<Event>& chronicle() const noexcept { return m_chronicle; }

private:
    // ---- фаза 1: команды Искры
    void phase_commands() {
        for (const Replay::Command& c : m_commands) {
            switch (static_cast<Cmd>(c.type)) {
            case Cmd::Move: {
                FVec3 dir{Fixed::from_raw(c.x), Fixed{}, Fixed::from_raw(c.z)};
                if (Math::length(dir) > Fixed::from_int(1)) dir = Math::normalize(dir);
                m_spark.wish = dir;
                break;
            }
            case Cmd::Give: {
                const Mana amount = Math::min(Mana::from_int(std::max<int>(c.arg, 0)), m_spark.reserve);
                if (amount.raw() <= 0) break;
                m_mana.inject(m_spark.pos, amount);
                m_spark.reserve -= amount;
                m_spark.gave += amount;
                m_spark.activity += amount.value / Fixed::from_int(8);
                break;
            }
            case Cmd::Take: {
                const Mana got = m_mana.draw(m_spark.pos, Fixed::from_int(4), Mana::from_int(std::max<int>(c.arg, 0)));
                m_spark.reserve = Math::min(Mana::from_int(1000), m_spark.reserve + got);
                m_spark.took += got;
                m_spark.activity += got.value / Fixed::from_int(8);
                break;
            }
            }
        }
    }

    // ---- фаза 2: движение и восстановление Искры
    void phase_spark() {
        m_spark.pos = Math::advance(m_spark.pos, m_spark.wish, Fixed::from_int(1));
        m_spark.pos.x = std::clamp<std::int64_t>(m_spark.pos.x, 0, world_size);
        m_spark.pos.z = std::clamp<std::int64_t>(m_spark.pos.z, 0, world_size);
        m_spark.reserve = Math::min(Mana::from_int(1000), m_spark.reserve + Mana::from_ratio(1, 2));
    }

    static void step_toward(WorldPos& pos, WorldPos target, Fixed speed) {
        const FVec3 d = Math::to_fvec(target - pos);
        const Fixed len = Math::length(d);
        if (len <= speed) { pos = target; return; }
        pos = Math::advance(pos, Math::normalize(d), speed);
    }
    static void clamp_world(WorldPos& pos) {
        pos.x = std::clamp<std::int64_t>(pos.x, 0, world_size);
        pos.z = std::clamp<std::int64_t>(pos.z, 0, world_size);
        pos.y = 0;
    }
    [[nodiscard]] Fixed relative_density(WorldPos pos) const { return m_mana.density(pos) / m_mana.config().base; }

    void log(EventKind kind, const Villager& who, const Villager* other = nullptr) {
        m_chronicle.push_back({m_tick, kind, who.name, other ? other->name : std::uint16_t{0}});
    }

    // ---- фаза 3: жители
    void phase_villagers() {
        std::vector<ECS::Entity> departed;
        const Fixed near_spark = Fixed::from_int(6);
        for (const ECS::Entity e : m_world.entities_of<Villager>()) {
            Villager& v = *m_world.get<Villager>(e);
            const std::uint32_t phase = m_tick + e.index;

            // Самочувствие: насколько хорошо там, где стоишь (плотность маны вокруг).
            const Fixed rel = relative_density(v.pos);
            const Fixed target = Math::clamp(rel, Fixed{}, Fixed::from_int(1));
            v.vitality += (target - v.vitality) / Fixed::from_int(32);
            if (v.vitality < Fixed::from_ratio(35, 100)) ++v.unease;
            else if (v.unease > 0) --v.unease;

            // Как житель читает Искру: дары и взятия рядом меняют доверие по-разному у разных укладов.
            const bool spark_near = Math::distance(v.pos, m_spark.pos) <= near_spark;
            if (spark_near) {
                const Fixed gave = m_spark.gave.value / Fixed::from_int(400), took = m_spark.took.value / Fixed::from_int(400);
                switch (v.way) {
                case Way::Extractor: v.trust += gave / Fixed::from_int(2) - took / Fixed::from_int(8); break;
                case Way::Reverent: v.trust += gave * Fixed::from_int(2) - took * Fixed::from_int(3); break;
                case Way::Fearful: v.trust -= (gave + took) / Fixed::from_int(2); break;
                case Way::Oblivious: v.trust += gave / Fixed::from_int(10); break;
                }
                v.trust = Math::clamp(v.trust, Fixed::from_int(-1), Fixed::from_int(1));
            }

            switch (v.way) {
            case Way::Extractor: {
                if (phase % 8 == 0) {
                    v.stock -= Math::min(v.stock, Mana::from_int(12)); // живут на добытом: расход идёт постоянно, поэтому давление на поле не прекращается
                    if (v.stock < extractor_stock_cap) {
                        const Mana want = Math::min(Mana::from_int(20), extractor_stock_cap - v.stock);
                        v.stock += m_mana.draw(v.pos, Fixed::from_int(3), want);
                    }
                }
                if (phase % 40 == 0 && v.stock > Mana::from_int(100)) { // десятина ближайшему чтущему
                    ECS::Entity best{};
                    Fixed best_d = Fixed::max();
                    for (const ECS::Entity o : m_world.entities_of<Villager>()) {
                        const Villager& ov = *m_world.get<Villager>(o);
                        if (ov.way != Way::Reverent) continue;
                        const Fixed d = Math::distance(v.pos, ov.pos);
                        if (d < best_d && d <= Fixed::from_int(12)) best = o, best_d = d;
                    }
                    if (best.index != 0) {
                        Villager& ov = *m_world.get<Villager>(best);
                        v.stock -= Mana::from_int(10), ov.stock += Mana::from_int(10);
                        log(EventKind::Tithe, v, &ov);
                    }
                }
                step_toward(v.pos, v.home, Fixed::from_ratio(3, 10));
                break;
            }
            case Way::Reverent: {
                if (phase % 16 == 0 && v.stock >= Mana::from_int(4)) { // дар в поле
                    m_mana.inject(v.pos, Mana::from_int(4));
                    v.stock -= Mana::from_int(4);
                    log(EventKind::Offering, v);
                }
                const Fixed to_spark = Math::distance(v.pos, m_spark.pos);
                if (v.flags & 2) { // паломник: идёт, пока верит, и остаётся рядом, дойдя (гистерезис: без дрожания у границы)
                    if (v.trust < Fixed::from_ratio(1, 10) || to_spark > Fixed::from_int(24)) v.flags &= static_cast<std::uint8_t>(~2u);
                    else if (to_spark > Fixed::from_int(3)) step_toward(v.pos, m_spark.pos, Fixed::from_ratio(4, 10));
                } else if (v.trust > Fixed::from_ratio(1, 5) && to_spark <= Fixed::from_int(20)) {
                    v.flags |= 2;
                    log(EventKind::Pilgrim, v);
                }
                if (!(v.flags & 2)) step_toward(v.pos, v.home, Fixed::from_ratio(2, 10));
                break;
            }
            case Way::Fearful: {
                const bool startled = spark_near && m_spark.activity > Fixed::from_ratio(1, 2);
                const bool still_afraid = (v.flags & 1) && m_spark.activity > Fixed::from_ratio(3, 20) && Math::distance(v.pos, m_spark.pos) <= Fixed::from_int(14);
                if (startled || still_afraid) { // гистерезис: испугался резко, успокаивается не сразу
                    if (!(v.flags & 1)) { v.flags |= 1; log(EventKind::Fled, v); }
                    const FVec3 away = Math::to_fvec(v.pos - m_spark.pos);
                    v.pos = Math::advance(v.pos, Math::normalize(away.x == Fixed{} && away.z == Fixed{} ? FVec3{Fixed::from_int(1), Fixed{}, Fixed{}} : away), Fixed::from_ratio(6, 10));
                } else {
                    v.flags &= static_cast<std::uint8_t>(~1u);
                    step_toward(v.pos, v.home, Fixed::from_ratio(3, 10));
                }
                break;
            }
            case Way::Oblivious: {
                if (rel < Fixed::from_ratio(93, 100)) { // обеднело: к ближайшему богатому месту (8 направлений, 4 м)
                    WorldPos best = v.pos;
                    Fixed best_rel = rel;
                    for (int d = 0; d < 8; ++d) {
                        static constexpr int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, dz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
                        WorldPos p{v.pos.x + dx[d] * 4 * meter, 0, v.pos.z + dz[d] * 4 * meter};
                        clamp_world(p);
                        const Fixed r = relative_density(p);
                        if (r > best_rel) best = p, best_rel = r;
                    }
                    if (best != v.pos) {
                        if (!(v.flags & 2)) { v.flags |= 2; log(EventKind::Migrated, v); }
                        step_toward(v.pos, best, Fixed::from_ratio(3, 10));
                    }
                } else {
                    v.flags &= static_cast<std::uint8_t>(~2u);
                }
                break;
            }
            }
            clamp_world(v.pos);
            if (v.unease > 300) departed.push_back(e);
        }
        for (const ECS::Entity e : departed) {
            log(EventKind::Departed, *m_world.get<Villager>(e));
            m_world.destroy(e);
        }
    }

    // ---- фаза 4: итоги тика
    void phase_aftermath() {
        m_spark.activity = m_spark.activity * Fixed::from_ratio(95, 100);
        m_spark.gave = {};
        m_spark.took = {};
    }

    Math::Rng m_rng;
    ManaField::ManaGrid m_mana;
    ECS::World m_world;
    Spark m_spark;
    Phases::Schedule m_schedule;
    std::span<const Replay::Command> m_commands;
    std::vector<Event> m_chronicle;
    std::uint32_t m_tick = 0;
};
static_assert(Replay::Simulatable<Village>);

// ------------------------------------------------------------------------------------------------ политики Искры

using Policy = std::function<void(const Village&, std::vector<Replay::Command>&)>;

/// Идёт к деревне и дарит ману понемногу, пока есть запас.
Policy generous_policy() {
    return [](const Village& v, std::vector<Replay::Command>& out) {
        const std::uint32_t t = v.tick_number();
        if (t == 0) out.push_back(move_command(Fixed{}, Fixed{}));
        if (t % 10 == 0 && v.spark().reserve > Mana::from_int(80)) out.push_back(give_command(10));
    };
}
/// Ходит между жителями и забирает ману.
Policy greedy_policy() {
    return [](const Village& v, std::vector<Replay::Command>& out) {
        const std::uint32_t t = v.tick_number();
        if (t % 6 == 0) out.push_back(take_command(20));
        if (t % 50 == 0) { // по кругу вокруг центра
            const int step = static_cast<int>((t / 50) % 4);
            out.push_back(move_command(Fixed::from_ratio(step % 2 == 0 ? 1 : -1, 2), Fixed::from_ratio(step < 2 ? 1 : -1, 2)));
        }
    };
}
Policy silent_policy() { return [](const Village&, std::vector<Replay::Command>&) {}; }
Policy wander_policy(std::uint64_t seed) {
    auto rng = std::make_shared<Math::Rng>(seed ^ 0xA11CE);
    return [rng](const Village& v, std::vector<Replay::Command>& out) {
        const std::uint32_t t = v.tick_number();
        if (t % 25 == 0) out.push_back(move_command(Fixed::from_ratio(static_cast<int>(rng->below(21)) - 10, 10), Fixed::from_ratio(static_cast<int>(rng->below(21)) - 10, 10)));
        const std::uint32_t roll = rng->below(40);
        if (roll == 0) out.push_back(give_command(25));
        if (roll == 1) out.push_back(take_command(25));
    };
}
Policy policy_by_name(const std::string& name, std::uint64_t seed) {
    if (name == "greedy") return greedy_policy();
    if (name == "silent") return silent_policy();
    if (name == "wander") return wander_policy(seed);
    return generous_policy();
}

// ------------------------------------------------------------------------------------------------ прогон и отчёт

struct Summary {
    int alive = 0;
    double vitality = 0.0;            ///< Среднее по живым.
    double reverent_trust = 0.0;      ///< Среднее доверие чтущих.
    int offerings = 0, tithes = 0, fled = 0, migrated = 0, pilgrims = 0, departed = 0;
    double reserve = 0.0;
    double field_excess = 0.0;        ///< Сумма (плотность − база) в загруженных ячейках: «насколько мир обеднён» (отрицательно — обеднён).
    Replay::StateHashes hashes;
};

Summary summarize(const Village& v) {
    Summary s;
    int reverent = 0;
    for (const ECS::Entity e : v.world().entities_of<Villager>()) {
        const Villager& vl = *v.world().get<Villager>(e);
        ++s.alive;
        s.vitality += vl.vitality.to_double();
        if (vl.way == Way::Reverent) s.reverent_trust += vl.trust.to_double(), ++reverent;
    }
    if (s.alive) s.vitality /= s.alive;
    if (reverent) s.reverent_trust /= reverent;
    for (const Event& ev : v.chronicle()) {
        switch (ev.kind) {
        case EventKind::Offering: ++s.offerings; break;
        case EventKind::Tithe: ++s.tithes; break;
        case EventKind::Fled: ++s.fled; break;
        case EventKind::Migrated: ++s.migrated; break;
        case EventKind::Pilgrim: ++s.pilgrims; break;
        case EventKind::Departed: ++s.departed; break;
        }
    }
    s.reserve = v.spark().reserve.to_double();
    s.field_excess = static_cast<double>(v.mana().excess_raw()) / static_cast<double>(Fixed::one_raw);
    s.hashes = v.hashes();
    return s;
}

/// Прогон через Replay::Driver (запись, повтор или без записи — один и тот же путь).
Summary run(std::uint64_t seed, std::uint32_t ticks, const Policy& policy, Replay::Session& session, std::string* report = nullptr, bool chronicle = false) {
    Village village(seed);
    Replay::Driver driver(village, session);
    std::vector<Replay::Command> live;
    for (std::uint32_t t = 0; t < ticks; ++t) {
        live.clear();
        if (!driver.replaying()) policy(village, live);
        if (!driver.step(live)) break;
    }
    const Summary s = summarize(village);
    if (report) {
        char buf[256];
        std::string out;
        std::snprintf(buf, sizeof buf, "\n  %-8s %-13s %5s %6s %6s %s\n", "имя", "уклад", "хорошо", "доверие", "запас", "где");
        out += buf;
        for (const ECS::Entity e : village.world().entities_of<Villager>()) {
            const Villager& v = *village.world().get<Villager>(e);
            const Fixed d = Math::distance(v.pos, v.home);
            std::snprintf(buf, sizeof buf, "  %-8s %-13s %5.2f %7.2f %6.0f  %s%.1f м от дома%s\n", name_of(v.name).c_str(), way_name(v.way), v.vitality.to_double(), v.trust.to_double(),
                          v.stock.to_double(), (v.flags & 1) ? "бежит, " : (v.flags & 2) ? "в пути, " : "", d.to_double(), "");
            out += buf;
        }
        std::snprintf(buf, sizeof buf, "  ушло из деревни: %d  | даров: %d, десятин: %d, бегств: %d, переселений: %d, паломничеств: %d\n", s.departed, s.offerings, s.tithes, s.fled, s.migrated, s.pilgrims);
        out += buf;
        std::snprintf(buf, sizeof buf, "  Искра: запас %.0f маны; мир (отклонение от базы): %+.0f; среднее самочувствие %.2f; доверие чтущих %+.2f\n", s.reserve, s.field_excess, s.vitality, s.reverent_trust);
        out += buf;
        if (chronicle) {
            out += "  летопись:\n";
            static const char* names[] = {"дар в поле", "десятина", "бегство", "переселение", "идёт к Искре", "ушёл из деревни"};
            std::size_t shown = 0;
            for (const Event& ev : village.chronicle()) {
                if (++shown > 40) { out += "    …\n"; break; }
                std::snprintf(buf, sizeof buf, "    %5.1f с  %-8s %s%s%s\n", static_cast<double>(ev.tick) / tick_hz, name_of(ev.who).c_str(), names[static_cast<int>(ev.kind)],
                              ev.other ? " → " : "", ev.other ? name_of(ev.other).c_str() : "");
                out += buf;
            }
        }
        *report = out;
    }
    return s;
}

struct Options {
    std::uint64_t seed = 1;
    std::uint32_t ticks = 1800;
    std::string policy = "generous";
    std::string record, replay;
    bool chronicle = false, check = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seed") o.seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--ticks") o.ticks = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
        else if (a == "--policy") o.policy = next();
        else if (a == "--record") o.record = next();
        else if (a == "--replay") o.replay = next();
        else if (a == "--chronicle") o.chronicle = true;
        else if (a == "--check") o.check = true;
    }
    return o;
}

int g_failed = 0;
void expect(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "ОШИБКА", what.c_str());
    if (!ok) ++g_failed;
}

int self_check(const Options& o) {
    std::printf("LivingVillage --check: сид %llu, %u тиков (%.0f с)\n", static_cast<unsigned long long>(o.seed), o.ticks, static_cast<double>(o.ticks) / tick_hz);
    Replay::CommandRegistry registry;
    register_commands(registry);

    // 1. Детерминизм: два прогона — одни и те же хеши по каждой подсистеме.
    Replay::Session off_a = Replay::Session::off(o.seed), off_b = Replay::Session::off(o.seed);
    const Summary a = run(o.seed, o.ticks, generous_policy(), off_a), b = run(o.seed, o.ticks, generous_policy(), off_b);
    expect(a.hashes == b.hashes, "детерминизм: два прогона дают одни хеши (" + a.hashes.describe() + ")");

    // 2. Запись и повтор: хеши совпали, движок не знает, откуда команды.
    const std::string path = "living_village_check.rec";
    {
        auto rec = Replay::Session::record(o.seed, path, &registry);
        expect(rec.has_value(), "запись открыта");
        if (!rec) return 1;
        const Summary r = run(o.seed, o.ticks, generous_policy(), *rec);
        const auto verdict = rec->finish(o.ticks, r.hashes);
        expect(verdict.has_value(), "запись закрыта");
    }
    auto loaded = Replay::Recording::load(path);
    expect(loaded.has_value() && loaded->complete, "запись читается и закончена штатно");
    if (loaded) {
        auto rep = Replay::Session::replay(*loaded);
        const Summary p = run(o.seed, o.ticks, silent_policy(), rep); // политика не нужна: команды берутся из записи
        const auto verdict = rep.finish(o.ticks, p.hashes).value();
        expect(verdict.match, "повтор: хеши совпали с записанными (" + std::to_string(loaded->command_count()) + " команд)");
    }
    std::remove(path.c_str());

    // 3. Деревня зависит от Искры: дарить и забирать — разные истории, и они читаемы.
    Replay::Session s1 = Replay::Session::off(o.seed), s2 = Replay::Session::off(o.seed), s3 = Replay::Session::off(o.seed);
    std::string r_gen, r_greedy, r_silent;
    const Summary gen = run(o.seed, o.ticks, generous_policy(), s1, &r_gen, o.chronicle);
    const Summary greedy = run(o.seed, o.ticks, greedy_policy(), s2, &r_greedy, o.chronicle);
    const Summary quiet = run(o.seed, o.ticks, silent_policy(), s3, &r_silent, o.chronicle);
    std::printf("\n— щедрая Искра:%s\n— жадная Искра:%s\n— молчащая Искра:%s\n\n", r_gen.c_str(), r_greedy.c_str(), r_silent.c_str());
    expect(gen.hashes.differing(greedy.hashes).size() >= 2, "щедрая и жадная Искры порождают разные деревни (разошлись: " + [&] {
        std::string s;
        for (const auto& n : gen.hashes.differing(greedy.hashes)) s += n + " ";
        return s;
    }() + ")");
    expect(gen.reverent_trust > greedy.reverent_trust + 0.2, "чтящие доверяют щедрой Искре заметно больше, чем жадной");
    expect(gen.field_excess > greedy.field_excess, "жадная Искра обедняет мир сильнее щедрой");
    expect(gen.offerings > 0 && gen.tithes > 0, "круговорот идёт: десятина и дары случаются сами (без Искры тоже: " + std::to_string(quiet.offerings) + " даров)");
    expect(gen.fled < greedy.fled || gen.pilgrims > greedy.pilgrims, "пугливые чаще бегут, а чтущие чаще идут к Искре — зависят от её поведения");
    expect(gen.alive >= 1 && greedy.alive >= 1 && quiet.alive >= 1, "деревня не вымирает целиком в этих сценариях");

    std::printf(g_failed == 0 ? "\nOK\n" : "\nОШИБКА: %d проверок не прошли\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    if (o.check) return self_check(o);

    Replay::CommandRegistry registry;
    register_commands(registry);
    Replay::Session session = Replay::Session::off(o.seed);
    if (!o.replay.empty()) {
        auto loaded = Replay::Recording::load(o.replay);
        if (!loaded) { std::printf("ОШИБКА: %s\n", loaded.error().c_str()); return 1; }
        session = Replay::Session::replay(std::move(*loaded));
    } else if (!o.record.empty()) {
        auto rec = Replay::Session::record(o.seed, o.record, &registry);
        if (!rec) { std::printf("ОШИБКА: %s\n", rec.error().c_str()); return 1; }
        session = std::move(*rec);
    }
    std::string report;
    const Summary s = run(o.seed, o.ticks, policy_by_name(o.policy, o.seed), session, &report, o.chronicle);
    std::printf("LivingVillage: сид %llu, Искра «%s», %u тиков (%.0f с)\n%s\nhashes: %s\n", static_cast<unsigned long long>(o.seed), o.policy.c_str(), o.ticks,
                static_cast<double>(o.ticks) / tick_hz, report.c_str(), s.hashes.describe().c_str());
    if (session.mode() != Replay::Session::Mode::Off) {
        const auto verdict = session.finish(o.ticks, s.hashes);
        if (!verdict) { std::printf("ОШИБКА: %s\n", verdict.error().c_str()); return 1; }
        std::printf("%s\n", Replay::describe(*verdict, session.mode(), o.ticks, 0).c_str());
    }
    return 0;
}
