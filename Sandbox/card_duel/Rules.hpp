#pragma once
/**
 * @file Rules.hpp
 * @brief CardDuel — правила карточной дуэли в духе Hearthstone: чистая логика без графики.
 *
 * Состояние партии (Match) — плоская структура фиксированного размера: тривиально копируется
 * и удовлетворяет MemorySystem::ZeroInitializable. Поэтому ИИ копирует партию тысячами в
 * арены потоков JobSystem, а нулевой Match — корректная «ещё не начатая» партия (ZII).
 *
 * Каждое изменение состояния записывается в журнал Outcome: из него игра делает события шины
 * (`duel.outcome`), а сцена — анимации. Симуляции ИИ журнал не ведут (nullptr).
 *
 * Детерминизм: случайность только из `Match::rng` (тасование колод). Одинаковые seed и действия
 * дают одинаковую партию на любой машине и при любом числе потоков.
 */

#include <EventSystem/EventSystem.hpp>
#include <JobSystem/JobSystem.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace CardDuel {

namespace es = EventSystem;

inline constexpr int max_hand = 10;     ///< Карт в руке; лишняя сгорает.
inline constexpr int max_board = 7;     ///< Существ на столе у одного игрока.
inline constexpr int max_deck = 30;     ///< Карт в колоде.
inline constexpr int max_mana = 10;     ///< Предел кристаллов маны.
inline constexpr int hero_health = 30;  ///< Здоровье героя.
inline constexpr int max_turns = 90;    ///< После этого хода — ничья (страховка автоигры).

using CardId = std::uint16_t;

/// @brief Вид карты.
enum class CardType : std::uint8_t { Minion, Spell };

/// @brief Особые свойства существа (битовая маска).
namespace Keyword {
inline constexpr std::uint8_t none = 0;
inline constexpr std::uint8_t taunt = 1 << 0;         ///< Провокация: атаковать можно только его.
inline constexpr std::uint8_t charge = 1 << 1;        ///< Рывок: атакует в ход появления.
inline constexpr std::uint8_t divine_shield = 1 << 2; ///< Божественный щит: первый урон поглощается.
} // namespace Keyword

/// @brief Эффект заклинания или боевого клича.
enum class Effect : std::uint8_t {
    None,
    Damage,             ///< amount урона цели.
    Heal,               ///< Восстановить amount здоровья цели.
    DamageEnemyMinions, ///< amount урона всем вражеским существам.
    DamageAllMinions,   ///< amount урона всем остальным существам.
    DamageEnemyHero,    ///< amount урона вражескому герою.
    DrawCards,          ///< Взять amount карт.
    Buff,               ///< Цели +amount атаки и +amount2 здоровья.
    Summon,             ///< Призвать существо `summon`.
};

/// @brief Кого можно выбрать целью.
enum class TargetRule : std::uint8_t { None, AnyCharacter, EnemyCharacter, FriendlyMinion, EnemyMinion, AnyMinion };

/// @brief Описание карты (неизменяемые данные, общие для всех экземпляров).
struct CardDef {
    std::string_view name;          ///< Название (UTF-8).
    std::string_view text;          ///< Текст карты.
    CardType type = CardType::Minion;
    std::uint8_t cost = 0;
    std::uint8_t attack = 0;
    std::uint8_t health = 0;
    std::uint8_t keywords = Keyword::none;
    Effect effect = Effect::None;   ///< Боевой клич существа или действие заклинания.
    std::int8_t amount = 0;
    std::int8_t amount2 = 0;
    TargetRule target = TargetRule::None;
    CardId summon = 0;              ///< Для Effect::Summon.
    std::uint32_t theme = 0;        ///< Цвет рисунка 0xRRGGBBAA.
    bool collectible = true;        ///< false — только призывается (жетон).
};

/// @brief Все карты игры; CardId — индекс.
[[nodiscard]] std::span<const CardDef> card_library() noexcept;
/// @brief Карта по номеру.
[[nodiscard]] const CardDef& card(CardId id) noexcept;
/// @brief Колода по умолчанию для игрока 0 или 1 (разные архетипы).
[[nodiscard]] std::vector<CardId> default_deck(int player);

/// @brief Список фиксированной ёмкости: тривиально копируется, нулевой — пустой.
template<typename T, std::size_t N>
struct FixedList {
    std::array<T, N> items{};
    std::uint16_t count = 0;

    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    [[nodiscard]] bool full() const noexcept { return count >= N; }
    [[nodiscard]] T& operator[](std::size_t i) noexcept { return items[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return items[i]; }
    [[nodiscard]] T* begin() noexcept { return items.data(); }
    [[nodiscard]] T* end() noexcept { return items.data() + count; }
    [[nodiscard]] const T* begin() const noexcept { return items.data(); }
    [[nodiscard]] const T* end() const noexcept { return items.data() + count; }
    [[nodiscard]] T& back() noexcept { return items[count - 1]; }

    bool push_back(const T& value) noexcept {
        if (full()) return false;
        items[count++] = value;
        return true;
    }
    void erase(std::size_t i) noexcept {
        for (std::size_t k = i; k + 1 < count; ++k) items[k] = items[k + 1];
        items[--count] = T{};
    }
    void clear() noexcept {
        items = {};
        count = 0;
    }
};

/// @brief Карта в руке.
struct HandCard {
    std::uint32_t uid = 0; ///< Уникальный номер экземпляра (0 — нет).
    CardId card = 0;
};

/// @brief Существо на столе. `uid` совпадает с uid карты, из которой оно сыграно.
struct Minion {
    std::uint32_t uid = 0;
    CardId card = 0;
    std::int16_t attack = 0;
    std::int16_t health = 0;
    std::int16_t max_health = 0;
    std::uint8_t keywords = Keyword::none;
    bool sleeping = false; ///< Только что сыграно (без рывка): атаковать нельзя.
    bool attacked = false; ///< Уже атаковало в этом ходу.

    [[nodiscard]] bool has(std::uint8_t keyword) const noexcept { return (keywords & keyword) != 0; }
    [[nodiscard]] bool can_attack() const noexcept { return attack > 0 && !sleeping && !attacked; }
};

/// @brief Игрок: герой, мана, колода, рука, стол.
struct Player {
    std::int16_t health = 0;
    std::int16_t max_health = 0;
    std::uint8_t mana = 0;
    std::uint8_t max_mana = 0;
    std::uint8_t fatigue = 0;
    FixedList<CardId, max_deck> deck;      ///< Верх колоды — последний элемент.
    FixedList<HandCard, max_hand> hand;
    FixedList<Minion, max_board> board;
};

/// @brief Итог партии.
enum class Result : std::uint8_t { None = 0, Player0 = 1, Player1 = 2, Draw = 3 };

/// @brief Партия целиком. Нулевая — не начатая.
struct Match {
    std::array<Player, 2> players{};
    std::uint8_t active = 0;      ///< Чей ход.
    std::uint16_t turn = 0;       ///< Номер хода (1 — первый ход игрока 0).
    std::uint32_t next_uid = 0;   ///< Следующий uid карты.
    std::uint64_t rng = 0;        ///< Состояние генератора (splitmix64).
    Result result = Result::None;

    [[nodiscard]] bool over() const noexcept { return result != Result::None; }
};

/// @brief uid героя игрока (у карт uid начинаются с 16).
[[nodiscard]] constexpr std::uint32_t hero_uid(int player) noexcept { return static_cast<std::uint32_t>(player) + 1; }
/// @brief Номер игрока по uid героя или -1.
[[nodiscard]] constexpr int hero_owner(std::uint32_t uid) noexcept { return uid == 1 ? 0 : uid == 2 ? 1 : -1; }

// =============================================================================
// Действия и журнал — они же события шины.
// =============================================================================

/// @brief Вид действия игрока.
enum class ActionKind : std::uint8_t { None = 0, PlayCard = 1, Attack = 2, EndTurn = 3 };

/**
 * @brief Действие (команда) игрока: сыграть карту, атаковать, закончить ход.
 *
 * Это же событие шины `duel.command`: его пишут PlayerInput и AI, читает Rules.
 */
struct Action {
    std::uint8_t kind = 0;      ///< ActionKind.
    std::uint8_t player = 0;    ///< Кто действует.
    std::uint32_t source = 0;   ///< uid карты в руке или атакующего существа.
    std::uint32_t target = 0;   ///< uid цели (герой — hero_uid) или 0.

    [[nodiscard]] ActionKind type() const noexcept { return static_cast<ActionKind>(kind); }
    bool operator==(const Action&) const noexcept = default;

    static constexpr std::string_view event_name = "duel.command";
    using fields = es::Fields<es::Field<"kind", &Action::kind>, es::Field<"player", &Action::player>,
                              es::Field<"source", &Action::source>, es::Field<"target", &Action::target>>;
};

/// @brief Вид записи журнала.
enum class OutcomeKind : std::uint8_t {
    None = 0,
    TurnStarted,    ///< player, value — мана, value2 — макс. мана, amount — номер хода.
    CardDrawn,      ///< player, uid, card.
    CardBurned,     ///< player, card — рука полна.
    Fatigue,        ///< player, amount — урон усталости (дальше идёт Damage).
    CardPlayed,     ///< player, uid, card, target, value — мана после.
    MinionSummoned, ///< player, uid, card, amount — атака, value — здоровье, keywords, slot.
    Attack,         ///< player, uid — атакующий, target.
    Damage,         ///< uid — цель, amount — урон, value — здоровье после.
    Heal,           ///< uid — цель, amount — лечение, value — здоровье после.
    ShieldPopped,   ///< uid — цель.
    Buff,           ///< uid — цель, amount — атака после, value — здоровье, value2 — макс. здоровье.
    MinionDied,     ///< player — владелец, uid, card.
    GameOver,       ///< amount — Result.
};

/**
 * @brief Запись журнала: что изменилось. Событие шины `duel.outcome`.
 */
struct Outcome {
    std::uint8_t kind = 0;     ///< OutcomeKind.
    std::uint8_t player = 0;
    std::uint8_t keywords = 0;
    std::uint8_t slot = 0;
    std::uint16_t card = 0;
    std::int16_t amount = 0;
    std::int16_t value = 0;
    std::int16_t value2 = 0;
    std::uint32_t uid = 0;
    std::uint32_t target = 0;

    [[nodiscard]] OutcomeKind type() const noexcept { return static_cast<OutcomeKind>(kind); }

    static constexpr std::string_view event_name = "duel.outcome";
    using fields = es::Fields<es::Field<"kind", &Outcome::kind>, es::Field<"player", &Outcome::player>,
                              es::Field<"keywords", &Outcome::keywords>, es::Field<"slot", &Outcome::slot>,
                              es::Field<"card", &Outcome::card>, es::Field<"amount", &Outcome::amount>,
                              es::Field<"value", &Outcome::value>, es::Field<"value2", &Outcome::value2>,
                              es::Field<"uid", &Outcome::uid>, es::Field<"target", &Outcome::target>>;
};

/// @brief Журнал; nullptr — не вести (симуляции ИИ).
using OutcomeLog = std::vector<Outcome>;

/// @brief Все допустимые действия: не больше 10 карт × 16 целей + 7 × 8 атак + конец хода.
using ActionList = FixedList<Action, 256>;

// =============================================================================
// Правила
// =============================================================================

/// @brief Новая партия: тасует колоды, раздаёт 3 и 4 карты, начинает ход игрока 0.
[[nodiscard]] Match start_match(std::uint64_t seed, std::span<const CardId> deck0, std::span<const CardId> deck1,
                                OutcomeLog* log = nullptr);

/// @brief Все допустимые действия активного игрока (пусто, если партия окончена).
void legal_actions(const Match& match, ActionList& out) noexcept;

/// @brief Допустимо ли действие сейчас.
[[nodiscard]] bool is_legal(const Match& match, const Action& action) noexcept;

/**
 * @brief Применяет действие.
 * @return `false` и ничего не меняет, если действие недопустимо.
 */
bool apply(Match& match, const Action& action, OutcomeLog* log = nullptr);

/// @brief Существо по uid (или nullptr) и его владелец.
[[nodiscard]] const Minion* find_minion(const Match& match, std::uint32_t uid, int* owner = nullptr) noexcept;
/// @brief Карта в руке по uid (или nullptr) и её владелец.
[[nodiscard]] const HandCard* find_hand_card(const Match& match, std::uint32_t uid, int* owner = nullptr) noexcept;
/// @brief Цели, которые подходят под правило для игрока `player`.
void valid_targets(const Match& match, int player, TargetRule rule, FixedList<std::uint32_t, 16>& out) noexcept;
/// @brief Нужна ли цель карте `card` в руке игрока сейчас (правило есть и цели существуют).
[[nodiscard]] bool needs_target(const Match& match, int player, CardId card) noexcept;

/// @brief Оценка позиции для игрока `player` (больше — лучше).
[[nodiscard]] float evaluate(const Match& match, int player) noexcept;

/// @brief Хеш состояния по полям (без байтов выравнивания) — контрольная сумма прогона.
[[nodiscard]] std::uint64_t hash(const Match& match) noexcept;

// =============================================================================
// ИИ: случайные продолжения хода, параллельно на JobSystem
// =============================================================================

/// @brief Параметры ИИ.
struct AiConfig {
    int rollouts = 48;         ///< Случайных продолжений хода на каждое кандидат-действие.
    int max_steps = 24;        ///< Предел действий в продолжении.
    std::uint64_t seed = 0;    ///< Добавка к генератору (разные ИИ — разный характер).
    std::size_t grain = 8;     ///< Симуляций в куске parallel_for.
};

/// @brief Решение ИИ и что ему стоило.
struct AiDecision {
    Action action{};               ///< Лучшее действие.
    float score = 0.0f;            ///< Его оценка.
    std::size_t candidates = 0;    ///< Сколько действий рассматривалось.
    std::size_t simulations = 0;   ///< Сколько партий сыграно до конца хода.
};

/**
 * @brief Выбирает действие активного игрока.
 *
 * Каждое допустимое действие проверяется `rollouts` случайными продолжениями хода; оценка действия —
 * лучшее продолжение (план хода) плюс малая доля среднего. Симуляции идут параллельно
 * (JobSystem::parallel_for), копии партии живут во временных аренах потоков (MemorySystem),
 * результаты пишутся в свои ячейки и сравниваются в фиксированном порядке — поэтому решение
 * одинаково при любом числе потоков.
 */
[[nodiscard]] AiDecision choose_action(const Match& match, JobSystem::Scheduler& jobs, const AiConfig& config = {});

} // namespace CardDuel
