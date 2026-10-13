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
 * **Нужда и последствия.** Каждый житель голоден: сытость падает сама и пополняется маной из ячеек вокруг. Мана в этом мире
 * восстанавливается медленно, а добывающие тянут её быстрее, чем она возвращается, — деревня сама подъедает своё место.
 * Голодный житель ищет место побогаче (и остаётся там, где ему стало лучше), слишком долгий голод заканчивается смертью,
 * слишком долгое «плохо» — уходом. Дары Искры спасают, взятия губят: это и есть последствия.
 *
 * Всё детерминированно (Fixed и целые, порядок жителей по id, `Math::Rng` с сидом): прогон записывается и воспроизводится
 * (`Replay`), хеши подсистем (`mana`, `villagers`, `spark`, `tick`) сходятся. Консольный: окна и GPU нет.
 *
 * Использование:
 *   LivingVillage [--seed N] [--ticks T] [--policy generous|greedy|silent|wander|healer] [--chronicle] [--record f | --replay f]
 *                 [--rival политика]   соперник-Искра (игрок 1) по тем же правилам: его команды идут в тот же поток
 *                 [--png файл.png] [--film каталог [--every N]]   картинка итога / кадр каждые N тиков (по умолчанию 100 = 10 с)
 *   LivingVillage --matrix N     таблица средних по N сидам для четырёх политик Искры
 *   LivingVillage --check        самопроверка (детерминизм, запись и повтор, «деревня зависит от Искры»); последняя строка `OK`
 */

#include <ECSSystem/ECSSystem.hpp>
#include <ManaField/ManaField.hpp>
#include <RendererSystem/Core/Image.hpp>
#include <Math/Math.hpp>
#include <Phases/Schedule.hpp>
#include <Replay/Replay.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

// Нужда и давление. Тик = 0,1 с: сытый житель голодает 60 с, умирает после 15 с полного голода.
const Fixed hunger_per_tick = Fixed::from_ratio(1, 450);
const Fixed hungry_below = Fixed::from_ratio(1, 2);           ///< Ниже — ест, при плохом месте — ищет еду.
const Mana meal_size = Mana::from_int(3);
const Fixed meal_nourishment = Fixed::from_ratio(1, 20);      ///< Сытость от полного приёма пищи.
const Fixed eatable_floor = Fixed::from_ratio(3, 4);          ///< Ниже этой доли базы мана слишком жидка, чтобы наесться.
constexpr std::uint32_t starve_limit_ticks = 150;
constexpr std::uint8_t flag_flee = 1, flag_move = 2, flag_forage = 4;

/// Поле этого мира: возвращается к базе медленно (1/320 за шаг вместо 1/80) — ресурс конечен, и «подъесть» его можно.
ManaField::Config village_field() {
    ManaField::Config c;
    c.relax = Fixed::from_ratio(1, 6000);
    c.base = Math::Mana::from_int(10);
    return c;
}

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
    std::uint8_t flags = 0;       ///< bit0 — бежит, bit1 — в пути (паломник/переселенец), bit2 — ищет еду (чтобы записывать в летопись только смену состояния).
    std::uint16_t name = 0;       ///< Код имени (два слога).
    Fixed vitality{};             ///< 0…1: насколько жителю хорошо там, где он стоит (плотность маны вокруг).
    Fixed trust{};                ///< −1…1: что он думает об Искре.
    Mana stock{};                 ///< Запас маны (у добывающих — добытое; у чтущих — для даров).
    WorldPos pos{};
    WorldPos home{};
    std::uint32_t unease = 0;     ///< Сколько тиков подряд жителю плохо; слишком долго — он уходит.
    Fixed fed = Fixed::from_int(1); ///< 0…1: сытость; падает сама, пополняется маной вокруг.
    std::uint32_t starve = 0;     ///< Сколько тиков подряд полный голод; слишком долго — смерть.
};

enum class EventKind : std::uint8_t { Offering, Tithe, Fled, Migrated, Pilgrim, Departed, Hungry, Starved };

struct Event {
    std::uint32_t tick = 0;
    EventKind kind = EventKind::Offering;
    std::uint16_t who = 0;     ///< Код имени.
    std::uint16_t other = 0;   ///< Код имени второго участника (десятина) или 0.
    Way way = Way::Oblivious;  ///< Уклад того, с кем случилось.
};

const char* const syllables[] = {"Ар", "Бе", "Ви", "Ге", "Да", "Ел", "Жи", "Зо", "Ин", "Ка", "Ло", "Ми", "На", "Ор", "Пи", "Ру"};
std::string name_of(std::uint16_t code) {
    if (code == 0) return "—";
    return std::string(syllables[code & 15]) + syllables[(code >> 4) & 15];
}

// ------------------------------------------------------------------------------------------------ симуляция

class Village {
public:
    /// @param spark_count Сколько Искр в деревне: 1 — игрок; 2 — и соперник по тем же правилам (команды с `Command::player == 1`).
    explicit Village(std::uint64_t seed, int spark_count = 1) : m_rng(seed), m_mana(village_field()), m_sparks(static_cast<std::size_t>(std::max(1, spark_count))) {
        const WorldPos centre{world_size / 2, 0, world_size / 2};
        m_sparks[0].pos = centre;
        m_sparks[0].reserve = Mana::from_int(600);
        for (std::size_t i = 1; i < m_sparks.size(); ++i) { // соперник начинает в углу, с таким же запасом
            m_sparks[i].pos = {world_size / 8, 0, world_size / 8};
            m_sparks[i].reserve = Mana::from_int(600);
        }
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
            villagers.add_signed(v.fed.raw), villagers.add(v.starve);
        }
        for (const Spark& sp : m_sparks) {
            spark.add_signed(sp.pos.x), spark.add_signed(sp.pos.z), spark.add_signed(sp.reserve.raw()), spark.add_signed(sp.activity.raw);
        }
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
    [[nodiscard]] const Spark& spark() const noexcept { return m_sparks.front(); }
    [[nodiscard]] const std::vector<Spark>& sparks() const noexcept { return m_sparks; }
    [[nodiscard]] const ECS::World& world() const noexcept { return m_world; }
    [[nodiscard]] const ManaField::ManaGrid& mana() const noexcept { return m_mana; }
    [[nodiscard]] const std::vector<Event>& chronicle() const noexcept { return m_chronicle; }

private:
    // ---- фаза 1: команды Искры
    void phase_commands() {
        for (const Replay::Command& c : m_commands) {
            if (c.player >= m_sparks.size()) continue; // нет такой Искры: отказ одинаков у всех
            Spark& m_spark = m_sparks[c.player];
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
                const Mana got = m_mana.draw(m_spark.pos, Fixed::from_int(8), Mana::from_int(std::max<int>(c.arg, 0)));
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
        for (Spark& sp : m_sparks) {
            sp.pos = Math::advance(sp.pos, sp.wish, Fixed::from_int(1));
            sp.pos.x = std::clamp<std::int64_t>(sp.pos.x, 0, world_size);
            sp.pos.z = std::clamp<std::int64_t>(sp.pos.z, 0, world_size);
            sp.reserve = Math::min(Mana::from_int(1000), sp.reserve + Mana::from_ratio(1, 2));
        }
    }

    /// Ближайшая к точке Искра: жители читают ту, что рядом (при одной Искре — её).
    [[nodiscard]] const Spark& nearest_spark(WorldPos pos) const {
        const Spark* best = &m_sparks.front();
        Fixed best_d = Math::distance(pos, best->pos);
        for (std::size_t i = 1; i < m_sparks.size(); ++i) {
            const Fixed d = Math::distance(pos, m_sparks[i].pos);
            if (d < best_d) best = &m_sparks[i], best_d = d;
        }
        return *best;
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
        m_chronicle.push_back({m_tick, kind, who.name, other ? other->name : std::uint16_t{0}, who.way});
    }

    // ---- фаза 3: жители
    void phase_villagers() {
        std::vector<ECS::Entity> departed, died;
        const Fixed near_spark = Fixed::from_int(6);
        for (const ECS::Entity e : m_world.entities_of<Villager>()) {
            Villager& v = *m_world.get<Villager>(e);
            const std::uint32_t phase = m_tick + e.index;

            // Самочувствие: насколько хорошо там, где стоишь (плотность маны вокруг).
            const Fixed rel = relative_density(v.pos);
            // Голод: сытость падает сама; голодный ест из ячеек вокруг (чем беднее поле, тем меньше съест).
            v.fed = Math::max(Fixed{}, v.fed - hunger_per_tick);
            if (v.fed < hungry_below && phase % 4 == 0 && rel >= eatable_floor) { // слишком жидкую ману не «поймать»
                const Mana got = m_mana.draw(v.pos, Fixed::from_int(3), meal_size);
                v.fed = Math::min(Fixed::from_int(1), v.fed + meal_nourishment * (got.value / meal_size.value));
            }
            // Самочувствие — это и место, и сытость: голодному плохо даже на богатой земле.
            const Fixed target = Math::clamp(Math::min(rel, v.fed + Fixed::from_ratio(1, 4)), Fixed{}, Fixed::from_int(1));
            v.vitality += (target - v.vitality) / Fixed::from_int(32);
            if (v.vitality < Fixed::from_ratio(35, 100)) ++v.unease;
            else if (v.unease > 0) --v.unease;
            if (v.fed <= Fixed::from_ratio(1, 50)) ++v.starve;
            else if (v.starve > 0) --v.starve;

            // Поиск еды: голодный в обеднённом месте идёт туда, где мана богаче; насытившись на новом месте, остаётся там (дом переезжает).
            bool foraging = false;
            if (v.fed < hungry_below && rel < Fixed::from_ratio(9, 10)) {
                WorldPos best = v.pos;
                Fixed best_rel = rel;
                for (int d = 0; d < 8; ++d) {
                    static constexpr int fdx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, fdz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
                    WorldPos p{v.pos.x + fdx[d] * 4 * meter, 0, v.pos.z + fdz[d] * 4 * meter};
                    clamp_world(p);
                    const Fixed r = relative_density(p);
                    if (r > best_rel) best = p, best_rel = r;
                }
                if (best != v.pos) {
                    foraging = true;
                    if (!(v.flags & flag_forage)) { v.flags |= flag_forage; log(EventKind::Hungry, v); }
                    step_toward(v.pos, best, Fixed::from_ratio(4, 10));
                }
            }
            if (!foraging && (v.flags & flag_forage) && v.fed > Fixed::from_ratio(4, 5)) {
                v.flags &= static_cast<std::uint8_t>(~flag_forage);
                v.home = v.pos; // осел на новом месте
            }

            // Как житель читает Искру (ближайшую): дары и взятия рядом меняют доверие по-разному у разных укладов.
            const Spark& m_spark = nearest_spark(v.pos);
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
                    v.stock -= Math::min(v.stock, Mana::from_int(6)); // живут на добытом: расход идёт постоянно, поэтому давление на поле не прекращается
                    if (v.stock < extractor_stock_cap) {
                        const Mana want = Math::min(Mana::from_int(10), extractor_stock_cap - v.stock);
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
                if (!foraging) step_toward(v.pos, v.home, Fixed::from_ratio(3, 10));
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
                if (!(v.flags & 2) && !foraging) step_toward(v.pos, v.home, Fixed::from_ratio(2, 10));
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
                    if (!foraging) step_toward(v.pos, v.home, Fixed::from_ratio(3, 10));
                }
                break;
            }
            case Way::Oblivious: {
                if (rel < Fixed::from_ratio(93, 100) && !foraging) { // обеднело: к ближайшему богатому месту (8 направлений, 4 м)
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
                } else if (!foraging) {
                    v.flags &= static_cast<std::uint8_t>(~2u);
                }
                break;
            }
            }
            clamp_world(v.pos);
            if (v.starve > starve_limit_ticks) died.push_back(e);
            else if (v.unease > 300) departed.push_back(e);
        }
        for (const ECS::Entity e : died) {
            log(EventKind::Starved, *m_world.get<Villager>(e));
            m_world.destroy(e);
        }
        for (const ECS::Entity e : departed) {
            log(EventKind::Departed, *m_world.get<Villager>(e));
            m_world.destroy(e);
        }
    }

    // ---- фаза 4: итоги тика
    void phase_aftermath() {
        for (Spark& sp : m_sparks) {
            sp.activity = sp.activity * Fixed::from_ratio(95, 100);
            sp.gave = {};
            sp.took = {};
        }
    }

    Math::Rng m_rng;
    ManaField::ManaGrid m_mana;
    ECS::World m_world;
    std::vector<Spark> m_sparks;
    Phases::Schedule m_schedule;
    std::span<const Replay::Command> m_commands;
    std::vector<Event> m_chronicle;
    std::uint32_t m_tick = 0;
};
static_assert(Replay::Simulatable<Village>);

// ------------------------------------------------------------------------------------------------ политики Искры

using Policy = std::function<void(const Village&, std::vector<Replay::Command>&)>;

/// Идёт к деревне и дарит ману понемногу, пока есть запас.
Policy generous_policy(std::size_t self = 0) {
    return [self](const Village& v, std::vector<Replay::Command>& out) {
        const std::uint32_t t = v.tick_number();
        if (t == 0) out.push_back(move_command(Fixed{}, Fixed{}));
        if (t % 10 == 0 && v.sparks()[self].reserve > Mana::from_int(80)) out.push_back(give_command(10));
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
/// Искра «видит» жителей в пределах 24 м (как мана — по тому, что с ними): идёт к самому голодному и подкармливает.
/// Это не чит: правила те же, что у остальных политик, — другое лишь решение, куда идти и кому дарить.
Policy healer_policy(std::size_t self = 0) {
    return [self](const Village& v, std::vector<Replay::Command>& out) {
        const Village::Spark& me = v.sparks()[self];
        const std::uint32_t t = v.tick_number();
        if (t % 5 != 0) return;
        const Villager* hungriest = nullptr;
        Fixed worst = Fixed::from_int(2);
        for (const ECS::Entity e : v.world().entities_of<Villager>()) {
            const Villager& vl = *v.world().get<Villager>(e);
            if (Math::distance(vl.pos, me.pos) > Fixed::from_int(24)) continue;
            if (vl.fed < worst) worst = vl.fed, hungriest = &vl;
        }
        if (hungriest == nullptr) {
            out.push_back(move_command(Fixed{}, Fixed{}));
            return;
        }
        const Fixed d = Math::distance(hungriest->pos, me.pos);
        if (d > Fixed::from_int(2)) {
            const FVec3 dir = Math::normalize(Math::to_fvec(hungriest->pos - me.pos));
            out.push_back(move_command(dir.x, dir.z));
        } else {
            out.push_back(move_command(Fixed{}, Fixed{}));
        }
        if (hungriest->fed < Fixed::from_ratio(3, 5) && me.reserve > Mana::from_int(40)) out.push_back(give_command(15));
    };
}
Policy policy_by_name(const std::string& name, std::uint64_t seed, std::size_t self = 0) {
    if (name == "healer") return healer_policy(self);
    if (name == "greedy") return greedy_policy();
    if (name == "silent") return silent_policy();
    if (name == "wander") return wander_policy(seed);
    return generous_policy(self);
}

// ------------------------------------------------------------------------------------------------ картинка

/// Вид сверху без окна и GPU: поле маны как «погода» (тёмное — пусто, голубое — база, белое — густо), жители точками уклада,
/// красное кольцо — голоден, фиолетовое — бежит, у каждого полоска сытости; Искра — белый ромб с кругом по недавней активности.
/// Масштаб 10 пикселей на метр: 64×64 м → 640×640.
RendererSystem::Image render_village(const Village& village) {
    using RendererSystem::Color;
    constexpr int px_per_m = 10;
    constexpr int size = 64 * px_per_m;
    RendererSystem::Image img(size, size, Color{8, 10, 18, 255});

    // Поле: ячейки по 2 м, цвет по плотности относительно базы.
    const Fixed base = village.mana().config().base.value;
    for (int cz = 0; cz < 32; ++cz) {
        for (int cx = 0; cx < 32; ++cx) {
            const WorldPos centre{(static_cast<std::int64_t>(cx) * 2 + 1) * meter, 0, (static_cast<std::int64_t>(cz) * 2 + 1) * meter};
            const float rel = std::clamp(static_cast<float>(village.mana().density(centre).value.to_double() / base.to_double()), 0.0f, 2.0f);
            const Color c = rel <= 1.0f ? Color::lerp(Color{20, 14, 28, 255}, Color{30, 90, 140, 255}, rel) : Color::lerp(Color{30, 90, 140, 255}, Color{225, 245, 255, 255}, rel - 1.0f);
            img.fill_rect(cx * 2 * px_per_m, cz * 2 * px_per_m, 2 * px_per_m, 2 * px_per_m, c);
        }
    }

    const auto to_px = [&](WorldPos p) { return std::pair<int, int>{static_cast<int>(p.x * px_per_m / meter), static_cast<int>(p.z * px_per_m / meter)}; };
    const auto disc = [&](int cx, int cy, int r, Color c, int inner = 0) {
        for (int y = -r; y <= r; ++y) {
            for (int x = -r; x <= r; ++x) {
                const int d2 = x * x + y * y;
                if (d2 > r * r || d2 < inner * inner) continue;
                const int ix = cx + x, iy = cy + y;
                if (ix >= 0 && iy >= 0 && ix < size && iy < size) img.set_pixel(ix, iy, c);
            }
        }
    };

    for (const ECS::Entity e : village.world().entities_of<Villager>()) {
        const Villager& v = *village.world().get<Villager>(e);
        const auto [x, y] = to_px(v.pos);
        if (v.fed < hungry_below) disc(x, y, 11, Color{235, 50, 50, 255}, 9);
        if (v.flags & flag_flee) disc(x, y, 11, Color{170, 90, 235, 255}, 9);
        Color c{};
        switch (v.way) {
        case Way::Extractor: c = Color{240, 150, 40, 255}; break;
        case Way::Reverent: c = Color{245, 215, 70, 255}; break;
        case Way::Fearful: c = Color{180, 120, 235, 255}; break;
        case Way::Oblivious: c = Color{110, 205, 120, 255}; break;
        }
        disc(x, y, 6, c);
        const int bar = std::clamp(static_cast<int>(v.fed.to_double() * 14.0), 0, 14);
        img.fill_rect(x - 7, y + 9, 14, 3, Color{40, 40, 50, 255});
        img.fill_rect(x - 7, y + 9, bar, 3, v.fed < hungry_below ? Color{235, 80, 70, 255} : Color{90, 220, 110, 255});
    }

    for (std::size_t k = 0; k < village.sparks().size(); ++k) {
        const Village::Spark& spark = village.sparks()[k];
        const Color color = k == 0 ? Color{255, 255, 255, 255} : Color{255, 120, 90, 255}; // игрок — белая, соперник — красноватая
        const auto [sx, sy] = to_px(spark.pos);
        const int glow = 8 + std::clamp(static_cast<int>(spark.activity.to_double() * 20.0), 0, 40);
        disc(sx, sy, glow, color, glow - 1);
        for (int i = -9; i <= 9; ++i) {
            const int w = 9 - std::abs(i);
            img.fill_rect(sx - w, sy + i, 2 * w + 1, 1, color);
        }
    }
    return img;
}

// ------------------------------------------------------------------------------------------------ прогон и отчёт

struct Summary {
    int alive = 0;
    double vitality = 0.0;            ///< Среднее по живым.
    double reverent_trust = 0.0;      ///< Среднее доверие чтущих.
    int offerings = 0, tithes = 0, fled = 0, migrated = 0, pilgrims = 0, departed = 0, hungry = 0, starved = 0;
    double fed = 0.0;                 ///< Средняя сытость живых.
    std::array<int, 4> left_by_way{}; ///< Ушедшие и умершие по укладам.
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
        s.fed += vl.fed.to_double();
        if (vl.way == Way::Reverent) s.reverent_trust += vl.trust.to_double(), ++reverent;
    }
    if (s.alive) s.vitality /= s.alive, s.fed /= s.alive;
    if (reverent) s.reverent_trust /= reverent;
    for (const Event& ev : v.chronicle()) {
        switch (ev.kind) {
        case EventKind::Offering: ++s.offerings; break;
        case EventKind::Tithe: ++s.tithes; break;
        case EventKind::Fled: ++s.fled; break;
        case EventKind::Migrated: ++s.migrated; break;
        case EventKind::Pilgrim: ++s.pilgrims; break;
        case EventKind::Departed: ++s.departed; ++s.left_by_way[static_cast<std::size_t>(ev.way)]; break;
        case EventKind::Hungry: ++s.hungry; break;
        case EventKind::Starved: ++s.starved; ++s.left_by_way[static_cast<std::size_t>(ev.way)]; break;
        }
    }
    s.reserve = v.spark().reserve.to_double();
    s.field_excess = static_cast<double>(v.mana().excess_raw()) / static_cast<double>(Fixed::one_raw);
    s.hashes = v.hashes();
    return s;
}

/// Прогон через Replay::Driver (запись, повтор или без записи — один и тот же путь).
Summary run(std::uint64_t seed, std::uint32_t ticks, const Policy& policy, Replay::Session& session, std::string* report = nullptr, bool chronicle = false,
            const std::function<void(const Village&)>& on_tick = {}, const Policy* rival = nullptr) {
    Village village(seed, rival != nullptr ? 2 : 1);
    Replay::Driver driver(village, session);
    std::vector<Replay::Command> live, rival_live;
    for (std::uint32_t t = 0; t < ticks; ++t) {
        live.clear();
        if (!driver.replaying()) {
            policy(village, live);
            if (rival != nullptr) { // соперник — второй игрок: его команды помечены номером 1 и идут в тот же поток
                rival_live.clear();
                (*rival)(village, rival_live);
                for (const Replay::Command& c : rival_live) live.push_back(c.with_player(1));
            }
        }
        if (!driver.step(live)) break;
        if (on_tick) on_tick(village);
    }
    const Summary s = summarize(village);
    if (report) {
        char buf[256];
        std::string out;
        std::snprintf(buf, sizeof buf, "\n  %-8s %-13s %6s %6s %7s %6s %s\n", "имя", "уклад", "хорошо", "сыт", "доверие", "запас", "где");
        out += buf;
        for (const ECS::Entity e : village.world().entities_of<Villager>()) {
            const Villager& v = *village.world().get<Villager>(e);
            const Fixed d = Math::distance(v.pos, v.home);
            std::snprintf(buf, sizeof buf, "  %-8s %-13s %6.2f %6.2f %7.2f %6.0f  %s%.1f м от дома%s\n", name_of(v.name).c_str(), way_name(v.way), v.vitality.to_double(), v.fed.to_double(),
                          v.trust.to_double(), v.stock.to_double(), (v.flags & flag_flee) ? "бежит, " : (v.flags & flag_forage) ? "ищет еду, " : (v.flags & flag_move) ? "в пути, " : "", d.to_double(), "");
            out += buf;
        }
        std::snprintf(buf, sizeof buf, "  живы: %d из %d | умерли от голода: %d, ушли: %d | даров: %d, десятин: %d, бегств: %d, переселений: %d, паломничеств: %d, поисков еды: %d\n",
                      s.alive, villager_count, s.starved, s.departed, s.offerings, s.tithes, s.fled, s.migrated, s.pilgrims, s.hungry);
        out += buf;
        std::snprintf(buf, sizeof buf, "  Искра: запас %.0f маны; мир (отклонение от базы): %+.0f; самочувствие %.2f, сытость %.2f; доверие чтущих %+.2f\n", s.reserve, s.field_excess, s.vitality, s.fed, s.reverent_trust);
        out += buf;
        if (chronicle) {
            out += "  летопись (без даров и десятин):\n";
            static const char* names[] = {"дар в поле", "десятина", "бегство", "переселение", "идёт к Искре", "ушёл из деревни", "голоден: ищет еду", "умер от голода"};
            std::size_t shown = 0;
            for (const Event& ev : village.chronicle()) {
                if (ev.kind == EventKind::Offering || ev.kind == EventKind::Tithe) continue; // рутина: счётчики есть в итоге
                if (++shown > 60) { out += "    …\n"; break; }
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
    std::string png, film, rival;
    std::uint32_t every = 100;
    int matrix = 0; ///< >0: сколько сидов прогнать для каждой политики (таблица средних).
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
        else if (a == "--png") o.png = next();
        else if (a == "--rival") o.rival = next();
        else if (a == "--film") o.film = next();
        else if (a == "--every") o.every = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10)));
        else if (a == "--matrix") o.matrix = std::max(1, std::atoi(next()));
    }
    return o;
}

/// Средние по сидам для одной политики.
struct Mean {
    double alive = 0, starved = 0, departed = 0, fed = 0, vitality = 0, trust = 0, excess = 0;
    std::array<double, 4> left_by_way{};
};

Mean mean_over_seeds(const std::string& policy, std::uint64_t first_seed, int seeds, std::uint32_t ticks) {
    Mean m;
    for (int i = 0; i < seeds; ++i) {
        const std::uint64_t seed = first_seed + static_cast<std::uint64_t>(i);
        Replay::Session off = Replay::Session::off(seed);
        const Summary s = run(seed, ticks, policy_by_name(policy, seed), off);
        m.alive += s.alive, m.starved += s.starved, m.departed += s.departed, m.fed += s.fed, m.vitality += s.vitality, m.trust += s.reverent_trust, m.excess += s.field_excess;
        for (std::size_t w = 0; w < 4; ++w) m.left_by_way[w] += s.left_by_way[w];
    }
    const double n = seeds;
    for (double& l : m.left_by_way) l /= n;
    m.alive /= n, m.starved /= n, m.departed /= n, m.fed /= n, m.vitality /= n, m.trust /= n, m.excess /= n;
    return m;
}

int print_matrix(const Options& o) {
    std::printf("LivingVillage --matrix: %d сидов с %llu, %u тиков\n", o.matrix, static_cast<unsigned long long>(o.seed), o.ticks);
    std::printf("  %-9s %6s %8s %6s %6s %9s %8s %9s   ушли: добыв. чтящ. пугл. незам.\n", "Искра", "живы", "умерли", "ушли", "сыт", "самочувст", "доверие", "мир");
    for (const char* policy : {"generous", "greedy", "silent", "wander", "healer"}) {
        const Mean m = mean_over_seeds(policy, o.seed, o.matrix, o.ticks);
        std::printf("  %-9s %6.2f %8.2f %6.2f %6.2f %9.2f %+8.2f %+9.0f   %13.2f %5.2f %5.2f %6.2f\n", policy, m.alive, m.starved, m.departed, m.fed, m.vitality, m.trust, m.excess,
                    m.left_by_way[0], m.left_by_way[1], m.left_by_way[2], m.left_by_way[3]);
    }
    return 0;
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

    // 2б. Партия двух Искр: команды соперника помечены игроком 1 и записываются в тот же поток — повтор воспроизводит и его.
    {
        const Policy mine = healer_policy(0), theirs = greedy_policy();
        Policy theirs_tagged = theirs;
        auto rec = Replay::Session::record(o.seed, path, &registry);
        if (!rec) return 1;
        const Summary r = run(o.seed, o.ticks, mine, *rec, nullptr, false, {}, &theirs_tagged);
        expect(rec->finish(o.ticks, r.hashes).has_value(), "запись партии с соперником закрыта");
        auto loaded2 = Replay::Recording::load(path);
        expect(loaded2.has_value(), "запись партии читается");
        if (loaded2) {
            std::size_t rival_commands = 0;
            for (const Replay::Command& c : loaded2->commands()) rival_commands += c.player == 1 ? 1 : 0;
            expect(rival_commands > 0, "в записи есть команды второго игрока (" + std::to_string(rival_commands) + ")");
            auto rep = Replay::Session::replay(*loaded2);
            const Summary p = run(o.seed, o.ticks, silent_policy(), rep, nullptr, false, {}, &theirs_tagged);
            expect(rep.finish(o.ticks, p.hashes).value().match, "повтор партии с соперником: хеши совпали");
        }
        std::remove(path.c_str());
    }

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
    expect(gen.hungry + greedy.hungry + quiet.hungry > 0, "нужда настоящая: жители голодают и ищут еду (" + std::to_string(gen.hungry + greedy.hungry + quiet.hungry) + " раз)");

    // 4. Не случайность одного сида: по трём сидам средние тянутся в ту же сторону.
    constexpr int seeds = 3;
    const Mean gm = mean_over_seeds("generous", o.seed, seeds, o.ticks), grm = mean_over_seeds("greedy", o.seed, seeds, o.ticks);
    std::printf("\n  по %d сидам: щедрая — доверие %+.2f, мир %+.0f, самочувствие %.2f | жадная — доверие %+.2f, мир %+.0f, самочувствие %.2f\n", seeds, gm.trust, gm.excess, gm.vitality,
                grm.trust, grm.excess, grm.vitality);
    expect(gm.trust > grm.trust + 0.5, "по сидам: чтящие доверяют щедрой Искре заметно больше");
    expect(gm.excess > grm.excess, "по сидам: жадная Искра обедняет мир сильнее");
    const Mean hm = mean_over_seeds("healer", o.seed, seeds, o.ticks);
    std::printf("  внимательная Искра (healer): живы %.2f из %d, доверие %+.2f, мир %+.0f\n", hm.alive, villager_count, hm.trust, hm.excess);
    expect(hm.alive >= gm.alive && hm.alive >= grm.alive, "по сидам: внимательная Искра сохраняет деревню не хуже слепых (щедрой и жадной)");

    // 5. Соперник по тем же правилам: жадный чародей-соперник уносит ману у деревни, которую подкармливает внимательная Искра.
    {
        double excess_alone = 0, excess_rival = 0, trust_alone = 0, trust_rival = 0;
        for (int i = 0; i < seeds; ++i) {
            const std::uint64_t seed = o.seed + static_cast<std::uint64_t>(i);
            const Policy healer = policy_by_name("healer", seed), rival = policy_by_name("greedy", seed, 1);
            Replay::Session off_alone = Replay::Session::off(seed), off_contested = Replay::Session::off(seed);
            const Summary alone = run(seed, o.ticks, healer, off_alone), contested = run(seed, o.ticks, healer, off_contested, nullptr, false, {}, &rival);
            excess_alone += alone.field_excess, excess_rival += contested.field_excess;
            trust_alone += alone.reverent_trust, trust_rival += contested.reverent_trust;
        }
        std::printf("\n  внимательная Искра одна: мир %+.0f, доверие %+.2f | с жадным соперником: мир %+.0f, доверие %+.2f (среднее по %d сидам)\n", excess_alone / seeds, trust_alone / seeds,
                    excess_rival / seeds, trust_rival / seeds, seeds);
        expect(excess_rival < excess_alone, "соперник обедняет мир, который кормит игрок: конфликт возникает из общего ресурса, без «злодея» в правилах");
    }
    // Самочувствие жителей за три минуты почти не зависит от Искры (деревня приспосабливается: ищет еду, переселяется): это вывод среза, а не проверка.

    std::printf(g_failed == 0 ? "\nOK\n" : "\nОШИБКА: %d проверок не прошли\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    if (o.check) return self_check(o);
    if (o.matrix > 0) return print_matrix(o);

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
    std::uint32_t frames = 0;
    const auto on_tick = [&](const Village& village) {
        if (o.film.empty() || village.tick_number() % o.every != 0) return;
        char name[512];
        std::snprintf(name, sizeof name, "%s/frame_%04u.png", o.film.c_str(), frames++);
        if (const auto saved = render_village(village).save_png(name); !saved) std::printf("ОШИБКА: %s\n", saved.error().c_str());
    };
    if (!o.film.empty()) std::filesystem::create_directories(o.film);
    // Итоговая картинка снимается внутри прогона, пока деревня жива (после `run` она уже разрушена).
    const std::uint32_t final_tick = o.ticks;
    const auto on_tick_png = [&](const Village& village) {
        on_tick(village);
        if (!o.png.empty() && village.tick_number() == final_tick) {
            if (const auto saved = render_village(village).save_png(o.png); !saved) std::printf("ОШИБКА: %s\n", saved.error().c_str());
        }
    };
    const Policy rival_policy = o.rival.empty() ? Policy{} : policy_by_name(o.rival, o.seed ^ 0xB1BAL, 1);
    const Summary s = run(o.seed, o.ticks, policy_by_name(o.policy, o.seed), session, &report, o.chronicle, on_tick_png, o.rival.empty() ? nullptr : &rival_policy);

    std::printf("LivingVillage: сид %llu, Искра «%s»%s%s%s, %u тиков (%.0f с)\n%s\nhashes: %s\n", static_cast<unsigned long long>(o.seed), o.policy.c_str(),
                o.rival.empty() ? "" : ", соперник «", o.rival.c_str(), o.rival.empty() ? "" : "»", o.ticks, static_cast<double>(o.ticks) / tick_hz, report.c_str(), s.hashes.describe().c_str());
    if (session.mode() != Replay::Session::Mode::Off) {
        const auto verdict = session.finish(o.ticks, s.hashes);
        if (!verdict) { std::printf("ОШИБКА: %s\n", verdict.error().c_str()); return 1; }
        std::printf("%s\n", Replay::describe(*verdict, session.mode(), o.ticks, 0).c_str());
    }
    return 0;
}
