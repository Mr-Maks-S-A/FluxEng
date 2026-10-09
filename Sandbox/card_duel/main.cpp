/**
 * @file main.cpp
 * @brief CardDuel — 3D-карточная дуэль в духе Hearthstone: витрина всех модулей и сторонних библиотек движка.
 *
 * Что откуда:
 * - **Core** — окно, фиксированный тик 60 Гц, ввод → события, хуки `frame()` / `render_3d()`, шрифт интерфейса;
 * - **RendererSystem** — Renderer3D (стол, карты, жетоны героев, частицы, свет снарядов, прозрачный щит),
 *   Camera3D (выбор карты лучом из курсора, подписи над объектами), MeshData (скруглённые карты),
 *   Framebuffer (лицевые стороны карт рисуются Renderer2D в текстуры с mip-уровнями), Font (stb_truetype,
 *   кириллица), Procedural (stb_perlin: рисунки карт, рубашка, дерево и сукно стола), Image::save_png (скриншот);
 * - **EventSystem** — команды игроков (`duel.command`), журнал правил (`duel.outcome`), эффекты (`duel.fx`);
 * - **ECSSystem** — сцена: каждая карта и герой — сущность (CardView, Transform, Shown, Lunge, Shake, Fade);
 * - **JobSystem** — ИИ проверяет каждое действие десятками случайных продолжений хода параллельно;
 * - **MemorySystem** — копии партии в аренах потоков (ИИ), частицы и снаряды в `Pool` (ZII);
 * - **WindowSystem** — курсор и кнопки мыши (через Core), клавиши → `platform.key`;
 * - **glm** — вся математика сцены, **stb** — изображения, шрифты, шум; **spdlog/fmt** — хроника партии в консоли.
 *
 * Граф событий:
 * ```
 *   Platform ── platform.key, platform.mouse_button ─▶ PlayerInput ─┐
 *                                                     AI ──────────┴─ duel.command ─▶ Rules ── duel.outcome ─▶ Director ── duel.fx ─▶ Effects
 *                                                                                                          └─▶ Chronicle
 * ```
 * Rules — единственный владелец партии (Match), Director — единственный, кто создаёт и уничтожает сущности сцены:
 * он проигрывает журнал правил с паузами, поэтому анимации идут по очереди, а сцена отстаёт от правил на «показ».
 *
 * Управление: ЛКМ по карте в руке — сыграть (если нужна цель — ЛКМ по цели), ЛКМ по своему существу — атаковать,
 * ПКМ — отмена, кнопка справа или E — конец хода, T — автоигра за вас, R — новая партия, P — пауза.
 * Аргументы: `--seed N`, `--autoplay` (ИИ за обоих), `--fast` (без пауз показа), `--reveal` (карты ИИ открыты),
 * `--rollouts N` (сила ИИ). Общие (`--ticks`, `--threads`, `--screenshot`) — см. Core::App.
 */

#include "Rules.hpp"

#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using InputSystem::Key;
using InputSystem::MouseButton;

namespace es = EventSystem;
namespace ms = MemorySystem;
namespace cd = CardDuel;
using namespace RendererSystem;

namespace {

// =============================================================================
// Стол: размеры и места
// =============================================================================

constexpr float pi = std::numbers::pi_v<float>;
constexpr glm::vec2 card_size{1.4f, 2.0f};
constexpr float card_thickness = 0.035f;
constexpr int face_width = 256;
constexpr int face_height = 366;
constexpr int hero_face_size = 256;
constexpr float board_scale = 0.85f;
constexpr float hand_tilt = -57.0f * pi / 180.0f; ///< Карты в руке смотрят на камеру.
constexpr std::uint32_t button_uid = 0xFFFFFF00u;  ///< «uid» кнопки конца хода при выборе мышью.

glm::vec3 hero_position(int player) { return {0.0f, 0.0f, player == 0 ? 3.2f : -3.2f}; }
/// Кристаллы маны — ряд справа от героя, ближе к центру стола.
glm::vec3 mana_position(int player, int index) {
    return hero_position(player) + glm::vec3{1.55f + 0.42f * static_cast<float>(index), 0.16f, player == 0 ? -0.95f : 0.95f};
}
glm::vec3 deck_position(int player) { return {7.1f, 0.0f, player == 0 ? 2.6f : -2.6f}; }
glm::vec3 button_position() { return {7.1f, 0.0f, 0.0f}; }
glm::vec3 stage_position() { return {-5.3f, 3.0f, 2.3f}; }
glm::vec3 preview_position() { return {-5.5f, 3.6f, 2.0f}; }

glm::vec3 rgb(Color c) { return glm::vec3(c.to_vec4()); }

std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
float random01(std::uint64_t& state) { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1ULL << 24); }

// =============================================================================
// Поза: положение, поворот, масштаб
// =============================================================================

struct Pose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

Pose mix(const Pose& a, const Pose& b, float t) {
    return Pose{glm::mix(a.position, b.position, t), glm::slerp(a.rotation, b.rotation, t), a.scale + (b.scale - a.scale) * t};
}

glm::mat4 matrix(const Pose& p) {
    return glm::translate(glm::mat4{1.0f}, p.position) * glm::mat4_cast(p.rotation) * glm::scale(glm::mat4{1.0f}, glm::vec3{p.scale});
}

glm::quat rotation_x(float radians) { return glm::angleAxis(radians, glm::vec3{1.0f, 0.0f, 0.0f}); }
const glm::quat face_up = rotation_x(-pi * 0.5f);  ///< Лицом вверх, верх карты — к сопернику.
const glm::quat face_down = rotation_x(pi * 0.5f); ///< Рубашкой вверх.

// =============================================================================
// Компоненты сцены (ECS). Нулевое значение — осмысленное (ZII).
// =============================================================================

enum class Zone : std::uint8_t { Hand, Board, Hero, Stage, Burn };

struct CardView {
    std::uint32_t uid = 0;
    cd::CardId card = 0;
    std::uint8_t owner = 0;
    Zone zone = Zone::Hand;
    bool face_down = false;
    bool hero = false;
};

/// base — куда сглаженно движется объект; now — base плюс анимации (выпад, дрожь, исчезание); prev — для интерполяции.
struct Transform {
    Pose base{};
    Pose now{};
    Pose prev{};
};

/// То, что сейчас показано на карте (отстаёт от правил на время показа).
struct Shown {
    std::int16_t attack = 0;
    std::int16_t health = 0;
    std::int16_t max_health = 0;
    std::uint8_t keywords = 0;
};

struct Lunge {
    glm::vec3 to{0.0f};
    int tick = 0;
    int duration = 0;
};
struct Shake {
    int ticks = 0;
};
struct Fade {
    int ticks = 0;
    int total = 1;
};
struct StageTimer {
    int ticks = 0;
};

// =============================================================================
// Событие эффектов: Director → Effects
// =============================================================================

enum class FxKind : std::uint8_t { Impact, Heal, Shield, Projectile, Summon, Death, Buff };

struct FxEvent {
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    std::int16_t amount = 0;
    std::uint32_t color = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;

    [[nodiscard]] glm::vec3 at() const { return {x, y, z}; }
    [[nodiscard]] glm::vec3 to() const { return {tx, ty, tz}; }

    static constexpr std::string_view event_name = "duel.fx";
    using fields = es::Fields<es::Field<"kind", &FxEvent::kind>, es::Field<"flags", &FxEvent::flags>,
                              es::Field<"amount", &FxEvent::amount>, es::Field<"color", &FxEvent::color>,
                              es::Field<"x", &FxEvent::x>, es::Field<"y", &FxEvent::y>, es::Field<"z", &FxEvent::z>,
                              es::Field<"tx", &FxEvent::tx>, es::Field<"ty", &FxEvent::ty>, es::Field<"tz", &FxEvent::tz>>;
};

FxEvent fx_at(FxKind kind, glm::vec3 at, std::uint32_t color, int amount = 0) {
    return FxEvent{.kind = static_cast<std::uint8_t>(kind), .amount = static_cast<std::int16_t>(amount), .color = color,
                   .x = at.x, .y = at.y, .z = at.z};
}

std::string_view player_name(int player) { return player == 0 ? "Вы" : "Противник"; }

// =============================================================================
// Rules: единственный владелец партии
// =============================================================================

struct RulesModule {
    es::EventReader<cd::Action> commands;
    es::EventWriter<cd::Outcome> out;
    cd::Match match{};
    cd::OutcomeLog log;
    std::uint64_t seed = 1;
    std::size_t applied = 0;
    std::size_t rejected = 0;
    bool ignore_commands_once = false;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Rules")
                                    .consumes<cd::Action>()
                                    .produces<cd::Outcome>(es::ChannelConfig{.reserve = 256, .max_events_per_tick = 4096});
        commands = bus.reader<cd::Action>(id);
        out = bus.writer<cd::Outcome>(id);
    }

    void start(std::uint64_t new_seed) {
        seed = new_seed;
        log.clear();
        match = cd::start_match(seed, cd::default_deck(0), cd::default_deck(1), &log);
        flush();
        ignore_commands_once = true; // команды прошлой партии ещё в шине
    }

    void flush() {
        for (const cd::Outcome& o : log) out.emit(o);
        log.clear();
    }

    void tick() {
        if (ignore_commands_once) {
            ignore_commands_once = false;
            return;
        }
        for (const cd::Action& action : commands.events()) {
            if (cd::apply(match, action, &log)) {
                ++applied;
            } else {
                ++rejected; // устаревшая команда (например, две подряд) — правила её просто не принимают
            }
        }
        flush();
    }
};

// =============================================================================
// Chronicle: хроника партии (spdlog в консоль и последние строки на экран)
// =============================================================================

struct Chronicle {
    es::EventReader<cd::Outcome> outcomes;
    std::unordered_map<std::uint32_t, cd::CardId> names; ///< uid → карта (для «Огр атакует Щитоносца»).
    std::deque<std::string> lines;
    std::size_t total = 0;
    bool ignore_once = false;

    void declare(es::EventBus& bus) { outcomes = bus.reader<cd::Outcome>(bus.declare_module("Chronicle").consumes<cd::Outcome>()); }

    void reset() {
        names.clear();
        lines.clear();
        ignore_once = true;
    }

    [[nodiscard]] std::string who(std::uint32_t uid) const {
        if (const int hero = cd::hero_owner(uid); hero >= 0) return hero == 0 ? "ваш герой" : "герой противника";
        const auto it = names.find(uid);
        return it == names.end() ? std::string("?") : std::format("«{}»", cd::card(it->second).name);
    }

    void add(std::string line) {
        spdlog::info("{}", line);
        lines.push_back(std::move(line));
        while (lines.size() > 9) lines.pop_front();
        ++total;
    }

    void tick() {
        if (ignore_once) {
            ignore_once = false;
            return;
        }
        for (const cd::Outcome& o : outcomes.events()) {
            switch (o.type()) {
                case cd::OutcomeKind::TurnStarted:
                    add(std::format("— Ход {}: {} (мана {}) —", o.amount, o.player == 0 ? "ваш" : "противника", o.value2));
                    break;
                case cd::OutcomeKind::CardDrawn: names[o.uid] = o.card; break;
                case cd::OutcomeKind::CardBurned: add(std::format("{}: «{}» сгорает — рука полна", player_name(o.player), cd::card(o.card).name)); break;
                case cd::OutcomeKind::Fatigue: add(std::format("{}: усталость, {} урона", player_name(o.player), o.amount)); break;
                case cd::OutcomeKind::CardPlayed:
                    names[o.uid] = o.card;
                    add(o.target != 0 ? std::format("{} разыгрывает «{}» → {}", player_name(o.player), cd::card(o.card).name, who(o.target))
                                      : std::format("{} разыгрывает «{}»", player_name(o.player), cd::card(o.card).name));
                    break;
                case cd::OutcomeKind::MinionSummoned: names[o.uid] = o.card; break;
                case cd::OutcomeKind::Attack: add(std::format("{} атакует: {}", who(o.uid), who(o.target))); break;
                case cd::OutcomeKind::Damage: add(std::format("  {} получает {} урона", who(o.uid), o.amount)); break;
                case cd::OutcomeKind::Heal: add(std::format("  {} восстанавливает {} здоровья", who(o.uid), o.amount)); break;
                case cd::OutcomeKind::ShieldPopped: add(std::format("  щит {} поглощает урон", who(o.uid))); break;
                case cd::OutcomeKind::Buff: add(std::format("  {} теперь {}/{}", who(o.uid), o.amount, o.value)); break;
                case cd::OutcomeKind::MinionDied: add(std::format("  {} погибает", who(o.uid))); break;
                case cd::OutcomeKind::GameOver: {
                    const auto result = static_cast<cd::Result>(o.amount);
                    add(result == cd::Result::Player0 ? "ПОБЕДА!" : result == cd::Result::Player1 ? "Поражение." : "Ничья.");
                    break;
                }
                case cd::OutcomeKind::None: break;
            }
        }
    }
};

// =============================================================================
// Director: проигрывает журнал правил на сцене. Единственный владелец сущностей ECS.
// =============================================================================

struct Director {
    es::EventReader<cd::Outcome> outcomes;
    es::EventWriter<FxEvent> fx;

    std::deque<cd::Outcome> queue;
    std::unordered_map<std::uint32_t, ECS::Entity> entities;
    std::array<std::vector<std::uint32_t>, 2> hand;
    std::array<std::vector<std::uint32_t>, 2> board;
    std::array<int, 2> mana{};
    std::array<int, 2> max_mana{};
    std::array<int, 2> deck{};
    int turn = 0;
    int active = 0;
    cd::Result result = cd::Result::None;
    std::string banner;
    int banner_ticks = 0;
    int wait = 0;
    bool fast = false;
    bool reveal = false;
    bool ignore_once = false;
    std::uint32_t next_temp_uid = 0xF0000000u;
    std::size_t played = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Director")
                                    .consumes<cd::Outcome>()
                                    .produces<FxEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024});
        outcomes = bus.reader<cd::Outcome>(id);
        fx = bus.writer<FxEvent>(id);
    }

    [[nodiscard]] bool busy() const { return !queue.empty() || wait > 0; }

    [[nodiscard]] ECS::Entity find(const ECS::World& world, std::uint32_t uid) const {
        const auto it = entities.find(uid);
        return it != entities.end() && world.valid(it->second) ? it->second : ECS::Entity{};
    }

    [[nodiscard]] glm::vec3 position_of(const ECS::World& world, std::uint32_t uid) const {
        const ECS::Entity e = find(world, uid);
        const Transform* t = e ? world.get<Transform>(e) : nullptr;
        return t != nullptr ? t->now.position + glm::vec3{0.0f, 0.3f, 0.0f} : glm::vec3{0.0f, 0.5f, 0.0f};
    }

    ECS::Entity spawn(ECS::World& world, std::uint32_t uid, cd::CardId card, int owner, Zone zone, const Pose& at) {
        const ECS::Entity e = world.create();
        const cd::CardDef& def = cd::card(card);
        world.emplace<CardView>(e, CardView{.uid = uid, .card = card, .owner = static_cast<std::uint8_t>(owner), .zone = zone});
        world.emplace<Transform>(e, Transform{at, at, at});
        world.emplace<Shown>(e, Shown{def.attack, def.health, def.health, def.keywords});
        entities[uid] = e;
        return e;
    }

    void reset(ECS::World& world) {
        world.clear();
        entities.clear();
        queue.clear();
        for (auto& h : hand) h.clear();
        for (auto& b : board) b.clear();
        mana = max_mana = {};
        deck = {static_cast<int>(cd::default_deck(0).size()), static_cast<int>(cd::default_deck(1).size())};
        turn = 0;
        active = 0;
        result = cd::Result::None;
        banner.clear();
        banner_ticks = 0;
        wait = 0;
        ignore_once = true; // журнал прошлой партии ещё в шине
        for (int p = 0; p < 2; ++p) {
            const Pose at{hero_position(p)};
            const ECS::Entity e = world.create();
            world.emplace<CardView>(e, CardView{.uid = cd::hero_uid(p), .owner = static_cast<std::uint8_t>(p), .zone = Zone::Hero, .hero = true});
            world.emplace<Transform>(e, Transform{at, at, at});
            world.emplace<Shown>(e, Shown{0, cd::hero_health, cd::hero_health, 0});
            entities[cd::hero_uid(p)] = e;
        }
    }

    void pace(int ticks) { wait = fast ? 0 : ticks; }

    void emit_fx(const FxEvent& event) { fx.emit(event); }

    void play(ECS::World& world, const cd::Outcome& o) {
        const int owner = o.player;
        switch (o.type()) {
            case cd::OutcomeKind::TurnStarted:
                active = owner;
                turn = o.amount;
                mana[static_cast<std::size_t>(owner)] = o.value;
                max_mana[static_cast<std::size_t>(owner)] = o.value2;
                banner = owner == 0 ? "Ваш ход" : "Ход противника";
                banner_ticks = 80;
                pace(30);
                break;
            case cd::OutcomeKind::CardDrawn: {
                const Pose at{deck_position(owner) + glm::vec3{0.0f, 0.5f, 0.0f}, face_down, 1.0f};
                const ECS::Entity e = spawn(world, o.uid, o.card, owner, Zone::Hand, at);
                world.get<CardView>(e)->face_down = owner == 1 && !reveal;
                hand[static_cast<std::size_t>(owner)].push_back(o.uid);
                --deck[static_cast<std::size_t>(owner)];
                pace(8);
                break;
            }
            case cd::OutcomeKind::CardBurned: {
                const Pose at{deck_position(owner) + glm::vec3{0.0f, 0.5f, 0.0f}, face_down, 1.0f};
                const ECS::Entity e = spawn(world, next_temp_uid++, o.card, owner, Zone::Burn, at);
                world.emplace<StageTimer>(e, StageTimer{fast ? 1 : 40});
                --deck[static_cast<std::size_t>(owner)];
                emit_fx(fx_at(FxKind::Death, stage_position(), 0xFF6A00FF));
                pace(20);
                break;
            }
            case cd::OutcomeKind::Fatigue:
                banner = std::format("{}: усталость ({})", player_name(owner), o.amount);
                banner_ticks = 60;
                pace(15);
                break;
            case cd::OutcomeKind::CardPlayed: {
                auto& h = hand[static_cast<std::size_t>(owner)];
                std::erase(h, o.uid);
                mana[static_cast<std::size_t>(owner)] = o.value;
                const ECS::Entity e = find(world, o.uid);
                const cd::CardDef& def = cd::card(o.card);
                if (e) {
                    CardView* view = world.get<CardView>(e);
                    view->face_down = false;
                    if (def.type == cd::CardType::Spell) {
                        view->zone = Zone::Stage;
                        world.emplace<StageTimer>(e, StageTimer{fast ? 1 : 45});
                    }
                }
                if (o.target != 0) {
                    FxEvent shot = fx_at(FxKind::Projectile, def.type == cd::CardType::Spell ? stage_position() : position_of(world, o.uid), def.theme);
                    const glm::vec3 to = position_of(world, o.target);
                    shot.tx = to.x, shot.ty = to.y, shot.tz = to.z;
                    emit_fx(shot);
                }
                pace(def.type == cd::CardType::Spell ? 28 : 8);
                break;
            }
            case cd::OutcomeKind::MinionSummoned: {
                ECS::Entity e = find(world, o.uid);
                if (!e) { // жетон (Скелет): появляется прямо на столе
                    const Pose at{hero_position(owner) * 0.35f + glm::vec3{0.0f, 0.4f, 0.0f}, face_up, 0.1f};
                    e = spawn(world, o.uid, o.card, owner, Zone::Board, at);
                }
                CardView* view = world.get<CardView>(e);
                view->zone = Zone::Board;
                view->face_down = false;
                *world.get<Shown>(e) = Shown{o.amount, o.value, o.value, o.keywords};
                auto& b = board[static_cast<std::size_t>(owner)];
                b.insert(b.begin() + std::min<std::ptrdiff_t>(o.slot, static_cast<std::ptrdiff_t>(b.size())), o.uid);
                emit_fx(fx_at(FxKind::Summon, glm::vec3{0.0f, 0.1f, owner == 0 ? 1.25f : -1.25f}, 0xD8C7A0FF));
                pace(10);
                break;
            }
            case cd::OutcomeKind::Attack:
                if (const ECS::Entity e = find(world, o.uid)) {
                    world.emplace<Lunge>(e, Lunge{position_of(world, o.target), 0, fast ? 2 : 22});
                }
                pace(11); // удар — на середине выпада
                break;
            case cd::OutcomeKind::Damage:
                if (const ECS::Entity e = find(world, o.uid)) {
                    world.get<Shown>(e)->health = o.value;
                    world.emplace<Shake>(e, Shake{16});
                }
                emit_fx(fx_at(FxKind::Impact, position_of(world, o.uid), 0xFF5A2AFF, o.amount));
                pace(9);
                break;
            case cd::OutcomeKind::Heal:
                if (const ECS::Entity e = find(world, o.uid)) world.get<Shown>(e)->health = o.value;
                emit_fx(fx_at(FxKind::Heal, position_of(world, o.uid), 0x66FF88FF, o.amount));
                pace(12);
                break;
            case cd::OutcomeKind::ShieldPopped:
                if (const ECS::Entity e = find(world, o.uid)) {
                    Shown* s = world.get<Shown>(e);
                    s->keywords = static_cast<std::uint8_t>(s->keywords & ~cd::Keyword::divine_shield);
                }
                emit_fx(fx_at(FxKind::Shield, position_of(world, o.uid), 0xFFD966FF));
                pace(10);
                break;
            case cd::OutcomeKind::Buff:
                if (const ECS::Entity e = find(world, o.uid)) {
                    Shown* s = world.get<Shown>(e);
                    s->attack = o.amount, s->health = o.value, s->max_health = o.value2;
                }
                emit_fx(fx_at(FxKind::Buff, position_of(world, o.uid), 0xFFE066FF));
                pace(14);
                break;
            case cd::OutcomeKind::MinionDied:
                std::erase(board[static_cast<std::size_t>(owner)], o.uid);
                if (const ECS::Entity e = find(world, o.uid)) world.emplace<Fade>(e, Fade{fast ? 1 : 26, fast ? 1 : 26});
                emit_fx(fx_at(FxKind::Death, position_of(world, o.uid), 0x9A9A9AFF));
                pace(14);
                break;
            case cd::OutcomeKind::GameOver:
                result = static_cast<cd::Result>(o.amount);
                banner = result == cd::Result::Player0 ? "ПОБЕДА" : result == cd::Result::Player1 ? "ПОРАЖЕНИЕ" : "НИЧЬЯ";
                banner_ticks = std::numeric_limits<int>::max();
                pace(0);
                break;
            case cd::OutcomeKind::None: break;
        }
    }

    void tick(ECS::World& world) {
        if (ignore_once) {
            ignore_once = false;
        } else {
            for (const cd::Outcome& o : outcomes.events()) queue.push_back(o);
        }

        // Показ разыгранного заклинания → исчезание → удаление.
        std::vector<ECS::Entity> to_fade;
        world.view<StageTimer>().each([&](ECS::Entity e, StageTimer& s) {
            if (--s.ticks <= 0) to_fade.push_back(e);
        });
        for (const ECS::Entity e : to_fade) {
            world.remove<StageTimer>(e);
            world.emplace<Fade>(e, Fade{fast ? 1 : 20, fast ? 1 : 20});
        }
        world.view<Fade, const CardView>().each([&](ECS::Entity e, Fade& f, const CardView& v) {
            if (--f.ticks <= 0) {
                entities.erase(v.uid);
                world.destroy(e); // текущую сущность удалять во время обхода можно
            }
        });

        if (banner_ticks > 0 && banner_ticks != std::numeric_limits<int>::max()) --banner_ticks;
        if (wait > 0) {
            --wait;
            return;
        }
        while (!queue.empty() && wait == 0) {
            const cd::Outcome o = queue.front();
            queue.pop_front();
            play(world, o);
            ++played;
        }
    }
};

// =============================================================================
// Effects: частицы, всплывающие числа, снаряды — в пулах MemorySystem
// =============================================================================

struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 color{0.0f};
    float size = 0.0f;
    int life = 0;
    int max_life = 0;
};

struct FloatingText {
    glm::vec3 position{0.0f};
    std::int32_t amount = 0;
    std::uint32_t color = 0;
    int life = 0;
    int max_life = 0;
};

struct Projectile {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    glm::vec3 color{0.0f};
    int tick = 0;
    int duration = 0;

    [[nodiscard]] glm::vec3 position() const {
        const float t = duration > 0 ? static_cast<float>(tick) / static_cast<float>(duration) : 1.0f;
        return glm::mix(from, to, t) + glm::vec3{0.0f, std::sin(pi * t) * 1.5f, 0.0f};
    }
};

template<typename T>
struct PoolList {
    ms::Pool<T> pool;
    std::vector<T*> live;

    explicit PoolList(std::size_t capacity) : pool(ms::Pool<T>::reserve(capacity)) {}

    T* add() {
        T* item = pool.allocate();
        if (item != nullptr) live.push_back(item);
        return item;
    }

    /// Удаляет элементы, для которых `dead(item)`; порядок оставшихся сохраняется (детерминированно).
    template<typename Pred>
    void sweep(Pred&& dead) {
        std::size_t kept = 0;
        for (T* item : live) {
            if (dead(*item)) {
                pool.free(item);
            } else {
                live[kept++] = item;
            }
        }
        live.resize(kept);
    }

    void clear() {
        for (T* item : live) pool.free(item);
        live.clear();
    }
};

struct Effects {
    es::EventReader<FxEvent> events;
    PoolList<Particle> particles{16384};
    PoolList<FloatingText> texts{256};
    PoolList<Projectile> projectiles{64};
    std::uint64_t rng = 0xC0FFEE;
    std::size_t spawned = 0;
    bool fast = false;

    void declare(es::EventBus& bus) { events = bus.reader<FxEvent>(bus.declare_module("Effects").consumes<FxEvent>()); }

    void burst(glm::vec3 at, glm::vec3 color, int count, float speed, float lift, float size, int life) {
        for (int i = 0; i < count; ++i) {
            Particle* p = particles.add();
            if (p == nullptr) return;
            const float angle = random01(rng) * 2.0f * pi;
            const float s = speed * (0.4f + random01(rng));
            *p = Particle{.position = at,
                          .velocity = {std::cos(angle) * s, lift * (0.5f + random01(rng)), std::sin(angle) * s},
                          .color = color * (0.7f + 0.5f * random01(rng)),
                          .size = size * (0.6f + 0.8f * random01(rng)),
                          .life = life,
                          .max_life = life};
            ++spawned;
        }
    }

    void number(glm::vec3 at, int amount, std::uint32_t color) {
        if (FloatingText* t = texts.add()) *t = FloatingText{at + glm::vec3{0.0f, 0.6f, 0.0f}, amount, color, 70, 70};
    }

    void clear() {
        particles.clear();
        texts.clear();
        projectiles.clear();
    }

    void tick() {
        for (const FxEvent& e : events.events()) {
            const glm::vec3 color = rgb(Color::from_rgba(e.color));
            switch (static_cast<FxKind>(e.kind)) {
                case FxKind::Impact:
                    burst(e.at(), color, 28, 0.08f, 0.10f, 0.11f, 40);
                    number(e.at(), -e.amount, 0xFF5544FF);
                    break;
                case FxKind::Heal:
                    burst(e.at(), color, 20, 0.03f, 0.08f, 0.09f, 50);
                    number(e.at(), e.amount, 0x66FF88FF);
                    break;
                case FxKind::Shield: burst(e.at(), color, 34, 0.10f, 0.06f, 0.10f, 36); break;
                case FxKind::Summon: burst(e.at(), color, 26, 0.09f, 0.02f, 0.10f, 30); break;
                case FxKind::Death: burst(e.at(), color, 30, 0.04f, 0.05f, 0.16f, 60); break;
                case FxKind::Buff: burst(e.at(), color, 18, 0.05f, 0.10f, 0.09f, 40); break;
                case FxKind::Projectile:
                    if (Projectile* p = projectiles.add()) *p = Projectile{e.at(), e.to(), color, 0, fast ? 1 : 18};
                    break;
            }
        }
        for (Particle* p : particles.live) {
            p->position += p->velocity;
            p->velocity *= 0.95f;
            p->velocity.y -= 0.004f;
            if (p->position.y < 0.05f) p->position.y = 0.05f, p->velocity.y = 0.0f;
            --p->life;
        }
        particles.sweep([](const Particle& p) { return p.life <= 0; });
        for (FloatingText* t : texts.live) t->position.y += 0.012f, --t->life;
        texts.sweep([](const FloatingText& t) { return t.life <= 0; });
        for (Projectile* p : projectiles.live) {
            ++p->tick;
            if (Particle* trail = particles.add()) {
                *trail = Particle{.position = p->position(), .color = p->color, .size = 0.12f, .life = 18, .max_life = 18};
            }
            if (p->tick >= p->duration) burst(p->to, p->color, 22, 0.07f, 0.08f, 0.1f, 30);
        }
        projectiles.sweep([](const Projectile& p) { return p.tick >= p.duration; });
    }
};

// =============================================================================
// Ввод игрока и ИИ: оба пишут duel.command
// =============================================================================

/// Команда прочитывается правилами в следующем тике, а журнал — сценой ещё через тик: ждём, пока он дойдёт.
struct CommandGate {
    es::Tick last = 0;
    bool sent = false;
    [[nodiscard]] bool ready(es::Tick now) const { return !sent || now >= last + 3; }
    void mark(es::Tick now) { last = now, sent = true; }
};

struct Selection {
    std::uint32_t source = 0; ///< uid карты в руке или атакующего существа; 0 — ничего не выбрано.
    bool attack = false;
};

struct PlayerInput {
    es::EventReader<Core::KeyEvent> keys;
    es::EventReader<Core::MouseButtonEvent> clicks;
    es::EventWriter<cd::Action> out;

    std::uint32_t hovered = 0;          ///< Под курсором (из Game::frame).
    glm::vec3 pointer{0.0f};            ///< Точка стола под курсором.
    Selection selection;
    std::string toast;
    int toast_ticks = 0;
    bool autoplay = false;
    bool restart = false;
    bool can_act = false;
    std::size_t commands = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("PlayerInput")
                                    .consumes<Core::KeyEvent>()
                                    .consumes<Core::MouseButtonEvent>()
                                    .produces<cd::Action>(es::ChannelConfig{.reserve = 16, .max_events_per_tick = 64});
        keys = bus.reader<Core::KeyEvent>(id);
        clicks = bus.reader<Core::MouseButtonEvent>(id);
        out = bus.writer<cd::Action>(id);
    }

    void say(std::string text) {
        toast = std::move(text);
        toast_ticks = 120;
    }

    void send(const cd::Action& action, CommandGate& gate, es::Tick now) {
        out.emit(action);
        gate.mark(now);
        selection = {};
        ++commands;
    }

    void left_click(const cd::Match& match, CommandGate& gate, es::Tick now) {
        if (hovered == button_uid) {
            send({.kind = static_cast<std::uint8_t>(cd::ActionKind::EndTurn), .player = 0}, gate, now);
            return;
        }
        if (selection.source != 0) {
            const cd::Action action{.kind = static_cast<std::uint8_t>(selection.attack ? cd::ActionKind::Attack : cd::ActionKind::PlayCard),
                                    .player = 0, .source = selection.source, .target = hovered};
            if (hovered != 0 && cd::is_legal(match, action)) {
                send(action, gate, now);
            } else {
                say(hovered == 0 ? "Отменено" : "Эту цель выбрать нельзя");
                selection = {};
            }
            return;
        }
        int owner = -1;
        if (const cd::HandCard* hc = cd::find_hand_card(match, hovered, &owner); hc != nullptr && owner == 0) {
            const cd::CardDef& def = cd::card(hc->card);
            const cd::Player& me = match.players[0];
            if (def.cost > me.mana) return say("Недостаточно маны");
            if (def.type == cd::CardType::Minion && me.board.full()) return say("На столе нет места");
            if (cd::needs_target(match, 0, hc->card)) {
                selection = {hc->uid, false};
                return;
            }
            const cd::Action play{.kind = static_cast<std::uint8_t>(cd::ActionKind::PlayCard), .player = 0, .source = hc->uid};
            if (cd::is_legal(match, play)) send(play, gate, now);
            else say("Это заклинание некуда направить");
            return;
        }
        if (const cd::Minion* m = cd::find_minion(match, hovered, &owner); m != nullptr && owner == 0) {
            if (m->can_attack()) selection = {m->uid, true};
            else say(m->attacked ? "Это существо уже атаковало" : m->attack <= 0 ? "У существа нет атаки" : "Существо ещё не готово (ход появления)");
        }
    }

    void tick(const cd::Match& match, bool director_busy, CommandGate& gate, es::Tick now) {
        can_act = !match.over() && match.active == 0 && !autoplay && !director_busy && gate.ready(now);
        if (toast_ticks > 0) --toast_ticks;
        for (const Core::KeyEvent& k : keys.events()) {
            if (!k.pressed()) continue;
            if (k.code() == Key::T) {
                autoplay = !autoplay;
                say(autoplay ? "Автоигра: ИИ играет за вас" : "Автоигра выключена");
                selection = {};
            } else if (k.code() == Key::R) {
                restart = true;
            } else if (k.code() == Key::E && can_act) {
                send({.kind = static_cast<std::uint8_t>(cd::ActionKind::EndTurn), .player = 0}, gate, now);
            }
        }
        for (const Core::MouseButtonEvent& c : clicks.events()) {
            if (!c.pressed()) continue;
            if (c.which() == MouseButton::Right) {
                selection = {};
            } else if (c.which() == MouseButton::Left && can_act) {
                left_click(match, gate, now);
            } else if (c.which() == MouseButton::Left && !match.over() && match.active == 1) {
                say("Сейчас ход противника");
            }
        }
        if (!can_act && selection.source != 0 && (match.active != 0 || match.over())) selection = {};
    }
};

struct AiPlayer {
    es::EventWriter<cd::Action> out;
    cd::AiConfig config{};
    int think_ticks = 35; ///< Пауза перед ходом — чтобы за ИИ можно было следить.
    int waited = 0;
    std::size_t decisions = 0;
    std::size_t simulations = 0;
    double total_ms = 0.0;
    double last_ms = 0.0;
    std::size_t last_simulations = 0;

    void declare(es::EventBus& bus) {
        out = bus.writer<cd::Action>(bus.declare_module("AI").produces<cd::Action>());
    }

    void tick(const cd::Match& match, bool director_busy, bool controls_player0, JobSystem::Scheduler& jobs, CommandGate& gate,
              es::Tick now) {
        const bool my_turn = match.active == 1 || controls_player0;
        if (match.over() || !my_turn || director_busy || !gate.ready(now)) {
            waited = 0;
            return;
        }
        if (++waited < think_ticks) return;
        waited = 0;
        const auto start = std::chrono::steady_clock::now();
        const cd::AiDecision decision = cd::choose_action(match, jobs, config);
        last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        total_ms += last_ms;
        last_simulations = decision.simulations;
        simulations += decision.simulations;
        ++decisions;
        out.emit(decision.action);
        gate.mark(now);
    }
};

/**
 * Бот ввода: играет за вас настоящими кликами мыши (Window::inject_*). Проверяет весь путь
 * «курсор → луч Camera3D → выбор карты → platform.mouse_button → PlayerInput → duel.command».
 */
struct InputBot {
    bool enabled = false;
    std::deque<glm::vec2> clicks; ///< Куда кликнуть (пиксели framebuffer'а).
    bool aimed = false;           ///< Курсор уже наведён — в следующем кадре нажимаем.
    int wait = 0;
    std::size_t clicks_done = 0;
    std::size_t plans = 0;
};

// =============================================================================
// Лица карт: Renderer2D рисует в Framebuffer, результат кэшируется по тому, что на карте написано
// =============================================================================

struct FaceCache {
    std::unordered_map<std::uint64_t, RenderTarget> faces;
    std::size_t renders = 0;

    template<typename Draw>
    const Texture& get(Renderer2D& r, std::uint64_t key, int width, int height, Draw&& draw) {
        if (const auto it = faces.find(key); it != faces.end()) return it->second.color();
        auto target = RenderTarget::create(r.device(), width, height,
                                           TargetDesc{.color = {.filter = TextureFilter::Linear, .wrap = TextureWrap::ClampToEdge, .mipmaps = true}});
        if (!target) throw std::runtime_error(target.error());
        target->bind();
        r.clear(Colors::transparent);
        r.begin(Camera2D{.position = {static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f},
                         .viewport = {static_cast<float>(width), static_cast<float>(height)}});
        draw(r);
        r.end();
        target->update_mipmaps();
        ++renders;
        return faces.emplace(key, std::move(*target)).first->second.color();
    }

    [[nodiscard]] const Texture* find(std::uint64_t key) const {
        const auto it = faces.find(key);
        return it != faces.end() ? &it->second.color() : nullptr;
    }

    void clear() { faces.clear(); }
};

std::uint64_t face_key(cd::CardId card, const Shown& s, bool hero) {
    const auto b = [](std::int16_t v) { return static_cast<std::uint64_t>(static_cast<std::uint8_t>(v)); };
    return std::uint64_t{card} | (b(s.attack) << 16) | (b(s.health) << 24) | (b(s.max_health) << 32) |
           (std::uint64_t{s.keywords} << 40) | (std::uint64_t{hero ? 1u : 0u} << 48);
}

// =============================================================================
// Игра
// =============================================================================

class CardDuelGame final : public Core::Game {
public:
    void setup(Core::App& app) override {
        spdlog::set_pattern("[%H:%M:%S] %v");
        std::uint64_t seed = 2026;
        const auto& args = app.config().extra_args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--seed" && i + 1 < args.size()) seed = std::strtoull(args[i + 1].c_str(), nullptr, 10);
            if (args[i] == "--autoplay") input.autoplay = true;
            if (args[i] == "--input-bot") bot.enabled = true;
            if (args[i] == "--fast") director.fast = effects.fast = true;
            if (args[i] == "--reveal") director.reveal = true;
            if (args[i] == "--rollouts" && i + 1 < args.size()) ai.config.rollouts = std::max(1, std::atoi(args[i + 1].c_str()));
        }
        if (director.fast) ai.think_ticks = 1;

        es::EventBus& bus = app.bus();
        rules.declare(bus);
        chronicle.declare(bus);
        director.declare(bus);
        effects.declare(bus);
        input.declare(bus);
        ai.declare(bus);

        create_resources(app);

        director.reset(world);
        chronicle.reset();
        director.ignore_once = chronicle.ignore_once = false; // первая партия: в шине ещё ничего нет
        rules.start(seed);
        rules.ignore_commands_once = false;
        std::println("CardDuel: seed {}, {} cards in library, AI rollouts {} on {} job threads", seed, cd::card_library().size(),
                     ai.config.rollouts, app.jobs().threads());
    }

    void frame(Core::App& app, float /*seconds*/) override {
        update_camera(app);
        drive_bot(app);
        pick(app);
        prepare_faces(app);
    }

    void tick(Core::App& app) override {
        if (input.restart) {
            input.restart = false;
            restart();
        }
        const es::Tick now = app.tick();
        rules.tick();
        chronicle.tick();
        director.tick(world);
        effects.tick();
        input.tick(rules.match, director.busy(), gate, now);
        ai.tick(rules.match, director.busy(), input.autoplay, app.jobs(), gate, now);
        layout();
    }

    void render_3d(Core::App& app, Renderer3D& r) override {
        const float alpha = app.tick_alpha();
        Environment env;
        env.ambient = Color{70, 72, 92, 255};
        env.sun = DirectionalLight{.direction = {-0.45f, -1.0f, -0.55f}, .color = Color{255, 241, 222, 255}, .intensity = 0.95f};
        env.fog_color = Color::from_rgba(0x0E0D14FF);
        env.fog_start = 18.0f;
        env.fog_end = 34.0f;
        const float flicker = 0.85f + 0.15f * std::sin(static_cast<float>(app.tick()) * 0.21f) * std::sin(static_cast<float>(app.tick()) * 0.077f);
        env.add_point({.position = {-8.4f, 1.6f, -5.6f}, .color = Color{255, 160, 70, 255}, .intensity = 1.1f * flicker, .radius = 10.0f});
        env.add_point({.position = {8.4f, 1.6f, -5.6f}, .color = Color{255, 160, 70, 255}, .intensity = 1.1f * flicker, .radius = 10.0f});
        for (const Projectile* p : effects.projectiles.live) {
            env.add_point({.position = p->position(), .color = Color::from_floats(p->color.r, p->color.g, p->color.b), .intensity = 2.0f, .radius = 4.0f});
        }

        r.begin(camera, env);
        draw_table(r, app);
        draw_cards(r, alpha);
        draw_effects(r);
        draw_arrow(r);
        r.end();
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        const glm::vec2 vp = app.camera().viewport;
        const FontHandle font = app.ui_font();
        const FontHandle bold = app.ui_font_bold();
        const TextStyle shadowed{.color = Colors::white, .layer = 20, .shadow = Color{0, 0, 0, 200}};

        // Сверху слева — ход и мана.
        r.draw_text(bold, std::format("Ход {}", std::max(director.turn, 1)), {16, 12}, {.size = 30, .color = Color::from_rgba(0xFFD978FF), .layer = 20, .shadow = Color{0, 0, 0, 200}});
        TextStyle info = shadowed;
        info.size = 20;
        r.draw_text(font, std::format("Мана {}/{} · колода {} · в руке у противника {}", director.mana[0], director.max_mana[0],
                                      director.deck[0], director.hand[1].size()), {16, 50}, info);
        if (bot.enabled) r.draw_text(font, "Бот ввода: клики мышью за вас", {16, 76}, {.size = 20, .color = Color::from_rgba(0x7FD8FFFF), .layer = 20, .shadow = Color{0, 0, 0, 200}});
        if (input.autoplay) r.draw_text(font, "Автоигра (T — выключить)", {16, 76}, {.size = 20, .color = Color::from_rgba(0x7FD8FFFF), .layer = 20, .shadow = Color{0, 0, 0, 200}});

        // Мана и здоровье — подписи у объектов сцены (Camera3D::world_to_screen).
        for (int p = 0; p < 2; ++p) {
            const glm::vec3 gems = mana_position(p, 10) + glm::vec3{0.1f, 0.05f, 0.0f};
            if (const ScreenPoint s = camera.world_to_screen(gems); s.visible) {
                r.draw_text(bold, std::format("{}/{}", director.mana[static_cast<std::size_t>(p)], director.max_mana[static_cast<std::size_t>(p)]),
                            s.position - glm::vec2{0.0f, 14.0f}, {.size = 24, .color = Color::from_rgba(0x8FC3FFFF), .layer = 20, .shadow = Color{0, 0, 0, 220}});
            }
        }

        // Хроника — слева посередине.
        const float chronicle_y = vp.y * 0.30f;
        r.fill_rect({{8.0f, chronicle_y - 8.0f}, {430.0f, 9.0f * 22.0f + 16.0f}}, Color{0, 0, 0, 110}, 18);
        float y = chronicle_y;
        for (const std::string& line : chronicle.lines) {
            r.draw_text(font, line, {16.0f, y}, {.size = 17, .color = Color{225, 222, 210, 255}, .layer = 19});
            y += 22.0f;
        }

        // Всплывающие числа урона и лечения.
        for (const FloatingText* t : effects.texts.live) {
            const ScreenPoint s = camera.world_to_screen(t->position);
            if (!s.visible) continue;
            const float k = static_cast<float>(t->life) / static_cast<float>(t->max_life);
            const Color c = Color::from_rgba(t->color).with_alpha(static_cast<std::uint8_t>(255.0f * std::min(1.0f, k * 2.0f)));
            r.draw_text(bold, std::format("{:+}", t->amount), s.position - glm::vec2{60.0f, 30.0f},
                        {.size = 46, .color = c, .align = TextAlign::Center, .max_width = 120, .layer = 30, .shadow = Color{0, 0, 0, c.a}});
        }

        // Баннер хода / итог партии.
        if (director.banner_ticks > 0 && !director.banner.empty()) {
            const bool final = director.result != cd::Result::None;
            const float k = final ? 1.0f : std::min(1.0f, static_cast<float>(director.banner_ticks) / 25.0f);
            const auto a = static_cast<std::uint8_t>(255.0f * k);
            r.fill_rect({{0.0f, vp.y * 0.40f - 10.0f}, {vp.x, final ? 130.0f : 90.0f}}, Color{0, 0, 0, static_cast<std::uint8_t>(130.0f * k)}, 24);
            r.draw_text(bold, director.banner, {0.0f, vp.y * 0.40f},
                        {.size = final ? 76.0f : 58.0f, .color = Color::from_rgba(0xFFD978FF).with_alpha(a), .align = TextAlign::Center,
                         .max_width = vp.x, .layer = 25, .shadow = Color{0, 0, 0, a}});
            if (final) {
                r.draw_text(font, "R — новая партия", {0.0f, vp.y * 0.40f + 86.0f},
                            {.size = 24, .color = Colors::white, .align = TextAlign::Center, .max_width = vp.x, .layer = 25});
            }
        }

        if (input.toast_ticks > 0) {
            const auto a = static_cast<std::uint8_t>(std::min(255, input.toast_ticks * 6));
            r.draw_text(bold, input.toast, {0.0f, vp.y * 0.58f},
                        {.size = 28, .color = Color{255, 196, 150, a}, .align = TextAlign::Center, .max_width = vp.x, .layer = 26, .shadow = Color{0, 0, 0, a}});
        }

        // ИИ и подсказка.
        r.draw_text(font, std::format("ИИ: {} симуляций за {:.2f} мс · {} потоков", ai.last_simulations, ai.last_ms, jobs_threads),
                    {vp.x - 520.0f, 48.0f}, {.size = 18, .color = Color{190, 200, 220, 255}, .align = TextAlign::Right, .max_width = 500, .layer = 20, .shadow = Color{0, 0, 0, 200}});
        r.draw_text(font, "ЛКМ — сыграть карту / атаковать · ПКМ — отмена · E — конец хода · T — автоигра · R — новая партия · P — пауза",
                    {0.0f, vp.y - 30.0f}, {.size = 17, .color = Color{170, 170, 180, 255}, .align = TextAlign::Center, .max_width = vp.x, .layer = 20});
    }

    [[nodiscard]] std::string status() const override {
        const cd::Match& m = rules.match;
        return std::format("turn {} | {} | hp {}:{} | AI {:.1f} ms", m.turn, m.over() ? "game over" : m.active == 0 ? "your turn" : "enemy turn",
                           m.players[0].health, m.players[1].health, ai.last_ms);
    }

    void shutdown(Core::App& app) override {
        const cd::Match& m = rules.match;
        std::println("\n===== CardDuel : summary =====");
        std::println("result: {} after {} turns | hero health {} vs {}",
                     m.result == cd::Result::Player0 ? "player wins" : m.result == cd::Result::Player1 ? "AI wins" : m.result == cd::Result::Draw ? "draw" : "unfinished",
                     m.turn, m.players[0].health, m.players[1].health);
        std::println("commands: {} applied, {} rejected | outcomes shown {} | chronicle lines {}", rules.applied, rules.rejected,
                     director.played, chronicle.total);
        std::println("player input commands: {} | input bot: {} plans, {} clicks", input.commands, bot.plans, bot.clicks_done);
        std::println("AI: {} decisions, {} simulations, {:.2f} ms per decision on {} job threads", ai.decisions, ai.simulations,
                     ai.decisions > 0 ? ai.total_ms / static_cast<double>(ai.decisions) : 0.0, app.jobs().threads());
        std::println("scene: {} entities | card faces rendered {} | particles spawned {} (live {})", world.alive(), faces.renders,
                     effects.spawned, effects.particles.live.size());
        std::uint64_t checksum = cd::hash(m);
        checksum = (checksum ^ director.played) * 1099511628211ULL;
        checksum = (checksum ^ world.alive()) * 1099511628211ULL;
        std::println("match checksum {:016x}", checksum);
        faces.clear();
    }

private:
    // ----------------------------------------------------------------- ресурсы

    void create_resources(Core::App& app) {
        Renderer2D& r = app.renderer();
        renderer2d = &r;
        target_uv = app.device().target_uv();
        jobs_threads = app.jobs().threads();
        circle = r.create_texture(Procedural::circle_image(128, Colors::white), {.filter = TextureFilter::Linear});
        ring = r.create_texture(Procedural::circle_image(128, Colors::white, 9.0f), {.filter = TextureFilter::Linear});

        for (const cd::CardDef& def : cd::card_library()) {
            const Color theme = Color::from_rgba(def.theme);
            const bool spell = def.type == cd::CardType::Spell;
            const auto id = static_cast<float>(art.size());
            Image image = Procedural::noise_image(
                {.width = 192, .height = 136, .scale = spell ? 2.5f : 3.5f, .z = id * 2.3f + 0.7f, .octaves = 5,
                 .kind = spell ? Procedural::NoiseKind::Turbulence : Procedural::NoiseKind::Ridge},
                {{0.0f, theme.scaled(0.18f)}, {0.45f, theme.scaled(0.7f)}, {0.8f, theme}, {1.0f, Color::lerp(theme, Colors::white, 0.6f)}});
            if (spell) { // магическая сфера в центре
                image.blend(Procedural::circle_image(84, Color::lerp(theme, Colors::white, 0.7f).with_alpha(170)), 54, 26);
                image.blend(Procedural::circle_image(100, Colors::white.with_alpha(200), 4.0f), 46, 18);
            }
            art.push_back(r.create_texture(image, {.filter = TextureFilter::Linear}));
        }
        const Color hero_themes[2] = {Color::from_rgba(0xF4D58DFF), Color::from_rgba(0xE0482BFF)};
        for (int p = 0; p < 2; ++p) {
            Image image = Procedural::noise_image({.width = 256, .height = 256, .scale = 3.0f, .z = 40.0f + static_cast<float>(p) * 9.0f,
                                                   .kind = Procedural::NoiseKind::Turbulence},
                                                  {{0.0f, hero_themes[p].scaled(0.2f)}, {0.6f, hero_themes[p]}, {1.0f, Colors::white}});
            hero_art[static_cast<std::size_t>(p)] = r.create_texture(image, {.filter = TextureFilter::Linear});
        }

        // Рубашка: тёмный шум, золотая рамка и эмблема.
        Image back = Procedural::noise_image({.width = face_width, .height = face_height, .scale = 3.0f, .z = 91.0f, .kind = Procedural::NoiseKind::Turbulence},
                                             {{0.0f, Color::from_rgba(0x0C1430FF)}, {0.6f, Color::from_rgba(0x24306EFF)}, {1.0f, Color::from_rgba(0x6E4FB0FF)}});
        const Color gold = Color::from_rgba(0xD9B45AFF);
        back.fill_rect(0, 0, face_width, 10, gold), back.fill_rect(0, face_height - 10, face_width, 10, gold);
        back.fill_rect(0, 0, 10, face_height, gold), back.fill_rect(face_width - 10, 0, 10, face_height, gold);
        back.blend(Procedural::circle_image(150, gold, 10.0f), (face_width - 150) / 2, (face_height - 150) / 2);
        back.blend(Procedural::circle_image(70, gold.with_alpha(200)), (face_width - 70) / 2, (face_height - 70) / 2);
        back_texture = r.create_texture(back, {.filter = TextureFilter::Linear, .mipmaps = true});

        wood = r.create_texture(Procedural::noise_image({.width = 512, .height = 512, .scale = 2.0f, .z = 3.0f, .octaves = 6},
                                                        {{0.0f, Color::from_rgba(0x2A160BFF)}, {0.5f, Color::from_rgba(0x5A341BFF)}, {1.0f, Color::from_rgba(0x8A5A30FF)}}),
                                {.filter = TextureFilter::Linear, .mipmaps = true});
        felt = r.create_texture(Procedural::noise_image({.width = 512, .height = 512, .scale = 14.0f, .z = 7.0f, .octaves = 3},
                                                        {{0.0f, Color::from_rgba(0x0F2A2CFF)}, {1.0f, Color::from_rgba(0x1F4A48FF)}}),
                                {.filter = TextureFilter::Linear, .mipmaps = true});

        card_body = Mesh::create(app.device(), MeshData::rounded_slab(card_size, card_thickness, 0.1f));
        card_face = Mesh::create(app.device(), MeshData::rounded_rect(card_size, 0.1f));
        card_glow = Mesh::create(app.device(), MeshData::rounded_rect(card_size + glm::vec2{0.18f}, 0.18f));
        hero_disc = Mesh::create(app.device(), MeshData::rounded_rect({1.9f, 1.9f}, 0.95f, 16));
    }

    void draw_face(Renderer2D& r, cd::CardId id, const Shown& s, FontHandle font, FontHandle bold) const {
        const cd::CardDef& def = cd::card(id);
        const bool spell = def.type == cd::CardType::Spell;
        const Color theme = Color::from_rgba(def.theme);
        const Color frame = spell ? Color::from_rgba(0x4B3A78FF) : Color::from_rgba(0x6B5235FF);
        const auto w = static_cast<float>(face_width);
        const auto h = static_cast<float>(face_height);

        r.fill_rect({{0, 0}, {w, h}}, frame.scaled(0.5f), 0);
        r.fill_rect({{7, 7}, {w - 14, h - 14}}, frame, 1);
        r.draw(SpriteInstance{.position = {22, 28}, .size = {212, 146}, .pivot = {0, 0}, .texture = art[id], .layer = 2});
        r.draw_rect({{22, 28}, {212, 146}}, 3, theme.scaled(0.55f), 3);

        r.fill_rect({{12, 168}, {w - 24, 40}}, Color::from_rgba(0x1C1813FF), 4);
        r.draw_rect({{12, 168}, {w - 24, 40}}, 2, theme, 5);
        r.draw_text(bold, def.name, {12, 176}, {.size = 21, .color = Colors::white, .align = TextAlign::Center, .max_width = w - 24, .layer = 6, .shadow = Colors::black});

        r.fill_rect({{20, 214}, {w - 40, 104}}, Color::from_rgba(0xE6D9BCFF), 4);
        r.draw_text(font, def.text, {26, 226}, {.size = 19, .color = Color::from_rgba(0x2A2118FF), .align = TextAlign::Center, .max_width = w - 52, .layer = 6});

        const auto gem = [&](glm::vec2 center, float size, Color fill, int value, Color text) {
            r.draw(SpriteInstance{.position = center, .size = {size, size}, .color = fill, .texture = circle, .layer = 7});
            r.draw(SpriteInstance{.position = center, .size = {size, size}, .color = Color{20, 16, 10, 255}, .texture = ring, .layer = 8});
            r.draw_text(bold, std::to_string(value), center - glm::vec2{size * 0.5f, size * 0.36f},
                        {.size = size * 0.62f, .color = text, .align = TextAlign::Center, .max_width = size, .layer = 9, .shadow = Colors::black});
        };
        gem({34, 34}, 58, Color::from_rgba(0x2F6FD6FF), def.cost, Colors::white);
        if (!spell) {
            const Color attack_text = s.attack > def.attack ? Color::from_rgba(0x7CFF7CFF) : Colors::white;
            const Color health_text = s.health < s.max_health ? Color::from_rgba(0xFF6B6BFF)
                                      : s.max_health > def.health ? Color::from_rgba(0x7CFF7CFF) : Colors::white;
            gem({36, h - 34}, 60, Color::from_rgba(0xE0A42CFF), s.attack, attack_text);
            gem({w - 36, h - 34}, 60, Color::from_rgba(0xC4302EFF), s.health, health_text);
        }
    }

    void draw_hero_face(Renderer2D& r, int player, const Shown& s, FontHandle bold) const {
        const auto size = static_cast<float>(hero_face_size);
        r.draw(SpriteInstance{.position = {0, 0}, .size = {size, size}, .pivot = {0, 0}, .texture = hero_art[static_cast<std::size_t>(player)], .layer = 0});
        r.draw(SpriteInstance{.position = {size * 0.5f, size * 0.5f}, .size = {size, size}, .color = Color::from_rgba(0xD9B45AFF), .texture = ring, .layer = 1});
        r.draw_text(bold, player == 0 ? "Светлая жрица" : "Повелитель огня", {0, 62},
                    {.size = 22, .color = Colors::white, .align = TextAlign::Center, .max_width = size, .layer = 2, .shadow = Colors::black});
        r.draw(SpriteInstance{.position = {size * 0.5f, size - 62}, .size = {84, 84}, .color = Color::from_rgba(0xC4302EFF), .texture = circle, .layer = 3});
        r.draw(SpriteInstance{.position = {size * 0.5f, size - 62}, .size = {84, 84}, .color = Color{20, 16, 10, 255}, .texture = ring, .layer = 4});
        const Color text = s.health < s.max_health ? Color::from_rgba(0xFFB0B0FF) : Colors::white;
        r.draw_text(bold, std::to_string(s.health), {size * 0.5f - 42, size - 62 - 30},
                    {.size = 50, .color = text, .align = TextAlign::Center, .max_width = 84, .layer = 5, .shadow = Colors::black});
    }

    void draw_button_face(Renderer2D& r, int state, FontHandle bold) const {
        const Color fill = state == 2 ? Color::from_rgba(0x2E8B3AFF) : state == 1 ? Color::from_rgba(0xC9A227FF) : Color::from_rgba(0x55575EFF);
        r.fill_rect({{0, 0}, {256, 128}}, fill.scaled(0.6f), 0);
        r.fill_rect({{8, 8}, {240, 112}}, fill, 1);
        r.draw_text(bold, state == 0 ? "ХОД ВРАГА" : "КОНЕЦ ХОДА", {0, 38},
                    {.size = 38, .color = Colors::white, .align = TextAlign::Center, .max_width = 256, .layer = 2, .shadow = Colors::black});
    }

    /// Всё, что видно на столе, должно иметь лицо до render_3d: рисуем недостающие в frame().
    void prepare_faces(Core::App& app) {
        Renderer2D& r = app.renderer();
        const FontHandle font = app.ui_font();
        const FontHandle bold = app.ui_font_bold();
        world.view<const CardView, const Shown>().each([&](const CardView& v, const Shown& s) {
            if (v.hero) {
                faces.get(r, face_key(static_cast<cd::CardId>(v.owner), s, true), hero_face_size, hero_face_size,
                          [&](Renderer2D& rr) { draw_hero_face(rr, v.owner, s, bold); });
            } else if (!v.face_down) {
                faces.get(r, face_key(v.card, s, false), face_width, face_height, [&](Renderer2D& rr) { draw_face(rr, v.card, s, font, bold); });
            }
        });
        for (int state = 0; state < 3; ++state) {
            faces.get(r, 0xB0B0000000000000ULL | static_cast<std::uint64_t>(state), 256, 128,
                      [&](Renderer2D& rr) { draw_button_face(rr, state, bold); });
        }
    }

    // ----------------------------------------------------------------- камера и выбор мышью

    void update_camera(Core::App& app) {
        const glm::vec2 vp = app.camera().viewport;
        const glm::vec2 mouse = app.input().mouse_screen;
        const float sway = vp.x > 0.0f ? (mouse.x / vp.x - 0.5f) : 0.0f; // лёгкий параллакс за курсором
        camera = Camera3D{.position = {sway * 0.6f, 12.8f, 9.6f}, .target = {sway * 0.3f, 0.0f, 0.75f},
                          .fov_y = 42.0f * pi / 180.0f, .near_plane = 0.1f, .far_plane = 60.0f, .viewport = vp};
    }

    void pick(Core::App& app) {
        const Ray ray = camera.screen_to_ray(app.input().mouse_screen);
        float best = std::numeric_limits<float>::max();
        std::uint32_t hit_uid = 0;
        const auto& hand = director.hand[0];
        world.view<const CardView, const Transform>().each([&](const CardView& v, const Transform& t) {
            if (v.zone == Zone::Stage || v.zone == Zone::Burn || (v.zone == Zone::Hand && v.owner != 0)) return;
            const Aabb box = v.hero ? Aabb{{-1.05f, -0.1f, -1.05f}, {1.05f, 0.4f, 1.05f}}
                                    : Aabb::from_center(glm::vec3{0.0f}, {card_size.x * 0.5f, card_size.y * 0.5f, 0.06f});
            Pose pose = t.now;
            if (v.zone == Zone::Hand) { // карта в руке выбирается по своему месту, а не по поднятой позе — иначе мигает
                const auto it = std::ranges::find(hand, v.uid);
                if (it != hand.end()) pose = target_pose(v, static_cast<std::size_t>(it - hand.begin()), hand.size(), false);
                if (input.hovered == v.uid) pose.position.y += 0.6f, pose.scale *= 1.15f; // поднятая карта чуть «шире» для курсора
            }
            if (const auto hit = intersect(ray.transformed(glm::inverse(matrix(pose))), box); hit && *hit < best) {
                best = *hit;
                hit_uid = v.uid;
            }
        });
        if (const auto hit = intersect(ray, Aabb::from_center(button_position() + glm::vec3{0.0f, 0.15f, 0.0f}, {0.95f, 0.2f, 0.5f}));
            hit && *hit < best) {
            hit_uid = button_uid;
        }
        input.hovered = hit_uid;
        if (const auto t = intersect(ray, Plane{{0.0f, 1.0f, 0.0f}, -0.3f})) input.pointer = ray.at(*t);
    }

    // ----------------------------------------------------------------- раскладка (тик)

    /// Где должен быть объект. `allow_hover = false` — место без подъёма под курсором (по нему и выбираем мышью).
    [[nodiscard]] Pose target_pose(const CardView& v, std::size_t slot, std::size_t count, bool allow_hover = true) const {
        const float offset = static_cast<float>(slot) - (static_cast<float>(count) - 1.0f) * 0.5f;
        const bool hovered = allow_hover && input.hovered == v.uid && director.result == cd::Result::None;
        switch (v.zone) {
            case Zone::Hand:
                if (v.owner == 0) {
                    const float spacing = std::min(1.3f, 8.5f / static_cast<float>(std::max<std::size_t>(count, 1)));
                    const float x = offset * spacing;
                    if (hovered) return Pose{{std::clamp(x, -5.5f, 5.5f), 3.7f, 4.9f}, rotation_x(hand_tilt), 1.6f};
                    return Pose{{x, 1.6f - std::abs(offset) * 0.05f, 5.55f + static_cast<float>(slot) * 0.012f},
                                rotation_x(hand_tilt) * glm::angleAxis(-offset * 0.05f, glm::vec3{0.0f, 0.0f, 1.0f}), 0.95f};
                } else {
                    const float spacing = std::min(1.0f, 7.0f / static_cast<float>(std::max<std::size_t>(count, 1)));
                    return Pose{{offset * spacing, 1.25f, -5.45f + static_cast<float>(slot) * 0.01f},
                                rotation_x(-50.0f * pi / 180.0f) * glm::angleAxis(pi, glm::vec3{0.0f, 1.0f, 0.0f}) *
                                    glm::angleAxis(offset * 0.05f, glm::vec3{0.0f, 0.0f, 1.0f}),
                                0.75f};
                }
            case Zone::Board:
                return Pose{{offset * 1.5f, hovered ? 0.3f : 0.06f, v.owner == 0 ? 1.25f : -1.25f}, face_up, hovered ? 0.92f : board_scale};
            case Zone::Hero: return Pose{hero_position(v.owner)};
            case Zone::Stage: return Pose{stage_position(), rotation_x(hand_tilt), 1.35f};
            case Zone::Burn: return Pose{stage_position() + glm::vec3{0.0f, 0.0f, v.owner == 0 ? 1.0f : -3.0f}, rotation_x(hand_tilt), 1.1f};
        }
        return {};
    }

    void layout() {
        std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> slots; // uid → (место, сколько)
        for (int p = 0; p < 2; ++p) {
            const auto& h = director.hand[static_cast<std::size_t>(p)];
            for (std::size_t i = 0; i < h.size(); ++i) slots[h[i]] = {i, h.size()};
            const auto& b = director.board[static_cast<std::size_t>(p)];
            for (std::size_t i = 0; i < b.size(); ++i) slots[b[i]] = {i, b.size()};
        }
        const float follow = director.fast ? 1.0f : 0.2f;
        std::vector<ECS::Entity> finished;
        world.view<const CardView, Transform>().each([&](ECS::Entity e, const CardView& v, Transform& t) {
            const auto it = slots.find(v.uid);
            const auto [slot, count] = it != slots.end() ? it->second : std::pair<std::size_t, std::size_t>{0, 1};
            t.prev = t.now;
            t.base = mix(t.base, target_pose(v, slot, count), follow);
            t.now = t.base;
            if (Lunge* l = world.get<Lunge>(e)) {
                const float k = std::sin(pi * static_cast<float>(l->tick) / static_cast<float>(std::max(l->duration, 1)));
                t.now.position += (l->to - t.base.position) * 0.78f * k + glm::vec3{0.0f, 0.7f * k, 0.0f};
                if (++l->tick > l->duration) finished.push_back(e);
            }
            if (Shake* s = world.get<Shake>(e); s != nullptr && s->ticks > 0) {
                t.now.position.x += std::sin(static_cast<float>(s->ticks) * 2.1f) * 0.08f * static_cast<float>(s->ticks) / 16.0f;
                --s->ticks;
            }
            if (const Fade* f = world.get<Fade>(e)) {
                const float k = static_cast<float>(f->ticks) / static_cast<float>(f->total);
                t.now.scale *= k;
                t.now.position.y -= (1.0f - k) * 0.3f;
            }
        });
        for (const ECS::Entity e : finished) world.remove<Lunge>(e);
    }

    // ----------------------------------------------------------------- отрисовка (кадр)

    void draw_table(Renderer3D& r, Core::App& app) {
        Renderer2D& r2 = app.renderer();
        r.draw_shape(Renderer3D::Shape::Cube, glm::translate(glm::mat4{1.0f}, {0.0f, -0.26f, 0.0f}) * glm::scale(glm::mat4{1.0f}, {17.6f, 0.5f, 12.8f}),
                     {.texture = &r2.texture(wood), .specular = 0.35f, .shininess = 40.0f});
        r.draw_shape(Renderer3D::Shape::Plane, glm::translate(glm::mat4{1.0f}, {0.0f, 0.001f, 0.0f}) * glm::scale(glm::mat4{1.0f}, {15.6f, 1.0f, 10.8f}),
                     {.texture = &r2.texture(felt), .specular = 0.05f});
        r.draw_shape(Renderer3D::Shape::Cube, glm::translate(glm::mat4{1.0f}, {0.0f, 0.005f, 0.0f}) * glm::scale(glm::mat4{1.0f}, {15.0f, 0.01f, 0.05f}),
                     {.color = Color::from_rgba(0xD9B45AFF), .emissive = {0.25f, 0.18f, 0.05f}});

        // Подсвечники по углам.
        for (const float x : {-8.4f, 8.4f}) {
            r.draw_shape(Renderer3D::Shape::Cylinder, glm::translate(glm::mat4{1.0f}, {x, 0.6f, -5.6f}) * glm::scale(glm::mat4{1.0f}, {0.35f, 1.2f, 0.35f}),
                         {.color = Color::from_rgba(0xEDE4CFFF)});
            r.draw_shape(Renderer3D::Shape::Sphere, glm::translate(glm::mat4{1.0f}, {x, 1.35f, -5.6f}) * glm::scale(glm::mat4{1.0f}, {0.18f, 0.3f, 0.18f}),
                         {.color = Color{255, 190, 90, 220}, .emissive = {1.6f, 0.9f, 0.3f}, .lit = false, .blend = BlendMode::Additive});
        }

        // Колоды: высота стопки — сколько карт осталось.
        for (int p = 0; p < 2; ++p) {
            const int count = director.deck[static_cast<std::size_t>(p)];
            if (count <= 0) continue;
            const float height = 0.022f * static_cast<float>(count);
            const Pose pile{deck_position(p) + glm::vec3{0.0f, height * 0.5f, 0.0f}, face_down, 1.0f};
            r.draw(card_body, matrix(pile) * glm::scale(glm::mat4{1.0f}, {1.0f, 1.0f, height / card_thickness}),
                   {.texture = &r2.texture(back_texture), .specular = 0.2f});
        }

        // Мана: заполненные кристаллы светятся.
        for (int p = 0; p < 2; ++p) {
            for (int i = 0; i < director.max_mana[static_cast<std::size_t>(p)]; ++i) {
                const bool full = i < director.mana[static_cast<std::size_t>(p)];
                const glm::vec3 at = mana_position(p, i);
                r.draw_shape(Renderer3D::Shape::Sphere, glm::translate(glm::mat4{1.0f}, at) * glm::scale(glm::mat4{1.0f}, glm::vec3{0.3f}),
                             {.color = full ? Color::from_rgba(0x4F9BFFFF) : Color::from_rgba(0x2A3140FF),
                              .emissive = full ? glm::vec3{0.1f, 0.35f, 0.9f} : glm::vec3{0.0f}, .specular = 0.9f, .shininess = 64.0f});
            }
        }

        // Кнопка конца хода.
        const bool idle = input.can_act;
        cd::ActionList options;
        cd::legal_actions(rules.match, options);
        const int state = !idle ? 0 : options.size() <= 1 ? 2 : 1;
        const bool hovered = input.hovered == button_uid;
        const float pulse = state == 2 ? 0.5f + 0.5f * std::sin(static_cast<float>(app.tick()) * 0.12f) : 0.0f;
        const glm::mat4 button = glm::translate(glm::mat4{1.0f}, button_position() + glm::vec3{0.0f, hovered ? 0.2f : 0.15f, 0.0f});
        r.draw_shape(Renderer3D::Shape::Cube, button * glm::scale(glm::mat4{1.0f}, {1.9f, 0.3f, 0.95f}),
                     {.color = Color::from_rgba(0x3A2E22FF), .specular = 0.3f});
        if (const Texture* label = faces.find(0xB0B0000000000000ULL | static_cast<std::uint64_t>(state))) {
            r.draw_shape(Renderer3D::Shape::Quad,
                         button * glm::translate(glm::mat4{1.0f}, {0.0f, 0.152f, 0.0f}) * glm::mat4_cast(face_up) *
                             glm::scale(glm::mat4{1.0f}, {1.8f, 0.86f, 1.0f}),
                         {.texture = label, .uv = target_uv, .emissive = glm::vec3{0.15f + 0.25f * pulse}, .specular = 0.4f});
        }
    }

    void draw_cards(Renderer3D& r, float alpha) {
        const cd::Match& m = rules.match;
        // Подсветка: что можно сыграть, кем атаковать, кого выбрать целью.
        cd::FixedList<std::uint32_t, 16> targets;
        if (input.selection.source != 0) {
            if (input.selection.attack) {
                cd::ActionList options;
                cd::legal_actions(m, options);
                for (const cd::Action& a : options) {
                    if (a.type() == cd::ActionKind::Attack && a.source == input.selection.source) targets.push_back(a.target);
                }
            } else if (const cd::HandCard* hc = cd::find_hand_card(m, input.selection.source)) {
                cd::valid_targets(m, 0, cd::card(hc->card).target, targets);
            }
        }
        const auto is_target = [&](std::uint32_t uid) { return std::find(targets.begin(), targets.end(), uid) != targets.end(); };

        std::optional<std::pair<cd::CardId, Shown>> preview;
        world.view<const CardView, const Transform, const Shown>().each([&](const CardView& v, const Transform& t, const Shown& s) {
            const Pose pose = mix(t.prev, t.now, alpha);
            const glm::mat4 model = matrix(pose);
            Color glow = Colors::transparent;
            if (is_target(v.uid)) glow = Color::from_rgba(0xFF4030FF);
            else if (input.selection.source == v.uid) glow = Color::from_rgba(0xFFD24AFF);
            else if (input.can_act && v.owner == 0) {
                if (v.zone == Zone::Hand) {
                    const cd::HandCard* hc = cd::find_hand_card(m, v.uid);
                    const cd::CardDef& def = cd::card(v.card);
                    if (hc != nullptr && def.cost <= m.players[0].mana && !(def.type == cd::CardType::Minion && m.players[0].board.full())) {
                        glow = Color::from_rgba(0x3CFF6EFF);
                    }
                } else if (v.zone == Zone::Board) {
                    if (const cd::Minion* mn = cd::find_minion(m, v.uid); mn != nullptr && mn->can_attack()) glow = Color::from_rgba(0x3CFF6EFF);
                }
            }

            if (v.hero) {
                r.draw_shape(Renderer3D::Shape::Cylinder, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.15f, 0.0f}) * glm::scale(glm::mat4{1.0f}, {2.1f, 0.3f, 2.1f}),
                             {.color = Color::from_rgba(0x6F6A62FF), .specular = 0.3f});
                if (glow.a > 0) {
                    r.draw_shape(Renderer3D::Shape::Cylinder, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.05f, 0.0f}) * glm::scale(glm::mat4{1.0f}, {2.5f, 0.1f, 2.5f}),
                                 {.color = glow, .emissive = rgb(glow), .lit = false});
                }
                if (const Texture* face = faces.find(face_key(static_cast<cd::CardId>(v.owner), s, true))) {
                    r.draw(hero_disc, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.302f, 0.0f}) * glm::mat4_cast(face_up),
                           {.texture = face, .uv = target_uv, .specular = 0.35f});
                }
                return;
            }

            const bool sleeping = v.zone == Zone::Board && [&] {
                const cd::Minion* mn = cd::find_minion(m, v.uid);
                return mn != nullptr && mn->sleeping && m.active == v.owner;
            }();
            const Color tint = sleeping ? Color{200, 200, 210, 255} : Colors::white;
            r.draw(card_body, model, {.color = tint, .texture = &app_texture(back_texture), .specular = 0.15f});
            if (!v.face_down) {
                if (const Texture* face = faces.find(face_key(v.card, s, false))) {
                    r.draw(card_face, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.0f, card_thickness * 0.5f + 0.003f}),
                           {.color = tint, .texture = face, .uv = target_uv, .specular = 0.3f});
                }
            }
            if (glow.a > 0) {
                r.draw(card_glow, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.0f, -card_thickness}),
                       {.color = glow, .emissive = rgb(glow) * 0.8f, .lit = false, .double_sided = true});
            }
            if (v.zone == Zone::Board) {
                if ((s.keywords & cd::Keyword::taunt) != 0) { // провокация — каменная рамка
                    r.draw(card_glow, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.0f, -card_thickness * 1.6f}) * glm::scale(glm::mat4{1.0f}, {1.12f, 1.1f, 1.0f}),
                           {.color = Color::from_rgba(0x8E8A84FF), .specular = 0.5f, .double_sided = true});
                }
                if ((s.keywords & cd::Keyword::divine_shield) != 0) { // божественный щит — прозрачный купол
                    r.draw_shape(Renderer3D::Shape::Sphere,
                                 glm::translate(glm::mat4{1.0f}, pose.position) * glm::scale(glm::mat4{1.0f}, {card_size.x * 1.2f * pose.scale, 0.7f, card_size.y * 1.15f * pose.scale}),
                                 {.color = Color{255, 214, 90, 70}, .emissive = {0.35f, 0.28f, 0.05f}, .lit = false, .blend = BlendMode::Additive, .double_sided = true});
                }
                if (input.hovered == v.uid && input.selection.source == 0) preview = std::pair{v.card, s};
            }
        });

        // Крупный показ существа под курсором.
        if (preview) {
            if (const Texture* face = faces.find(face_key(preview->first, preview->second, false))) {
                const glm::mat4 model = matrix(Pose{preview_position(), rotation_x(hand_tilt), 1.55f});
                r.draw(card_body, model, {.texture = &app_texture(back_texture)});
                r.draw(card_face, model * glm::translate(glm::mat4{1.0f}, {0.0f, 0.0f, card_thickness * 0.5f + 0.003f}),
                       {.texture = face, .uv = target_uv, .emissive = glm::vec3{0.08f}});
            }
        }
    }

    void draw_effects(Renderer3D& r) {
        for (const Particle* p : effects.particles.live) {
            const float k = static_cast<float>(p->life) / static_cast<float>(std::max(p->max_life, 1));
            r.draw_shape(Renderer3D::Shape::Cube, glm::translate(glm::mat4{1.0f}, p->position) * glm::scale(glm::mat4{1.0f}, glm::vec3{p->size * (0.4f + 0.6f * k)}),
                         {.color = Color::from_floats(p->color.r, p->color.g, p->color.b, k), .emissive = p->color * k,
                          .lit = false, .blend = BlendMode::Additive});
        }
        for (const Projectile* p : effects.projectiles.live) {
            r.draw_shape(Renderer3D::Shape::Sphere, glm::translate(glm::mat4{1.0f}, p->position()) * glm::scale(glm::mat4{1.0f}, glm::vec3{0.45f}),
                         {.color = Color::from_floats(p->color.r, p->color.g, p->color.b), .emissive = p->color * 1.5f, .lit = false});
        }
    }

    void draw_arrow(Renderer3D& r) {
        if (input.selection.source == 0) return;
        const glm::vec3 from = director.position_of(world, input.selection.source);
        glm::vec3 to = input.pointer + glm::vec3{0.0f, 0.2f, 0.0f};
        if (input.hovered != 0 && input.hovered != button_uid) to = director.position_of(world, input.hovered);
        constexpr int steps = 18;
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const glm::vec3 p = glm::mix(from, to, t) + glm::vec3{0.0f, std::sin(pi * t) * 1.4f, 0.0f};
            const float size = i == steps ? 0.4f : 0.13f + 0.05f * t;
            r.draw_shape(Renderer3D::Shape::Sphere, glm::translate(glm::mat4{1.0f}, p) * glm::scale(glm::mat4{1.0f}, glm::vec3{size}),
                         {.color = Color::from_rgba(0xFF3A2AFF), .emissive = {0.9f, 0.15f, 0.08f}, .lit = false});
        }
    }

    // ----------------------------------------------------------------- бот ввода

    void plan_bot_clicks(Core::App& app) {
        const cd::AiDecision decision = cd::choose_action(rules.match, app.jobs(), ai.config);
        ++bot.plans;
        const auto screen = [&](glm::vec3 point) { return camera.world_to_screen(point).position; };
        const cd::Action& a = decision.action;
        switch (a.type()) {
            case cd::ActionKind::EndTurn: bot.clicks.push_back(screen(button_position() + glm::vec3{0.0f, 0.3f, 0.0f})); break;
            case cd::ActionKind::PlayCard: {
                const auto& hand = director.hand[0];
                const auto it = std::ranges::find(hand, a.source);
                if (it == hand.end()) return;
                const CardView view{.uid = a.source, .owner = 0, .zone = Zone::Hand};
                bot.clicks.push_back(screen(target_pose(view, static_cast<std::size_t>(it - hand.begin()), hand.size(), false).position));
                if (a.target != 0) bot.clicks.push_back(screen(director.position_of(world, a.target)));
                break;
            }
            case cd::ActionKind::Attack:
                bot.clicks.push_back(screen(director.position_of(world, a.source)));
                bot.clicks.push_back(screen(director.position_of(world, a.target)));
                break;
            case cd::ActionKind::None: break;
        }
    }

    void drive_bot(Core::App& app) {
        if (!bot.enabled) return;
        if (bot.wait > 0) {
            --bot.wait;
            return;
        }
        if (bot.clicks.empty()) {
            if (!input.can_act) return;
            plan_bot_clicks(app);
            if (bot.clicks.empty()) return;
        }
        WindowSystem::Window& window = app.window();
        const WindowSystem::Size ws = window.window_size();
        const WindowSystem::Size fb = window.framebuffer_size();
        const glm::vec2 to_window{fb.width > 0 ? static_cast<float>(ws.width) / static_cast<float>(fb.width) : 1.0f,
                                  fb.height > 0 ? static_cast<float>(ws.height) / static_cast<float>(fb.height) : 1.0f};
        if (!bot.aimed) { // кадр 1: навести курсор (выбор под курсором обновится в следующем кадре)
            const glm::vec2 at = bot.clicks.front() * to_window;
            window.inject_cursor(at.x, at.y);
            bot.aimed = true;
            return;
        }
        window.inject_mouse_button(MouseButton::Left, InputSystem::Transition::Press); // кадр 2: клик
        window.inject_mouse_button(MouseButton::Left, InputSystem::Transition::Release);
        bot.clicks.pop_front();
        bot.aimed = false;
        bot.wait = director.fast ? 3 : 20;
        ++bot.clicks_done;
    }

    // ----------------------------------------------------------------- партия

    void restart() {
        effects.clear();
        faces.clear(); // лица с повреждениями прошлой партии больше не нужны
        director.reset(world);
        chronicle.reset();
        input.selection = {};
        input.say("Новая партия");
        rules.start(rules.seed + 1);
    }

    [[nodiscard]] const Texture& app_texture(TextureHandle handle) const { return renderer2d->texture(handle); }

    ECS::World world;
    RulesModule rules;
    Chronicle chronicle;
    Director director;
    Effects effects;
    PlayerInput input;
    AiPlayer ai;
    InputBot bot;
    CommandGate gate;
    FaceCache faces;
    Camera3D camera{};

    const Renderer2D* renderer2d = nullptr;
    std::vector<TextureHandle> art;
    std::array<TextureHandle, 2> hero_art{};
    TextureHandle circle{}, ring{}, back_texture{}, wood{}, felt{};
    Mesh card_body, card_face, card_glow, hero_disc;
    UvRect target_uv{}; ///< Как рисовать текстуры целей (лица карт) на этом бэкенде.
    unsigned jobs_threads = 0;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<CardDuelGame>({.title = "CardDuel", .width = 1600, .height = 900, .ticks_per_second = 60.0, .pause_key = InputSystem::Key::P,
                                    .camera_controls = false, .clear_rgba = 0x0E0D14FF},
                                   argc, argv);
}
