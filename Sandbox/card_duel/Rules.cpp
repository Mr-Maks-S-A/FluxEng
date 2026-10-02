#include "Rules.hpp"

#include <MemorySystem/MemorySystem.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace CardDuel {

static_assert(MemorySystem::ZeroInitializable<Match>, "Match must live in arenas: trivially copyable and destructible");
static_assert(es::Event<Action>);
static_assert(es::Event<Outcome>);

namespace {

// =============================================================================
// Карты
// =============================================================================

constexpr CardId skeleton = 14;

constexpr std::array<CardDef, 23> library{{
    // --- существа
    {.name = "Новобранец", .text = "", .cost = 1, .attack = 1, .health = 2, .theme = 0x8A9BB0FF},
    {.name = "Гончая", .text = "Рывок", .cost = 1, .attack = 1, .health = 1, .keywords = Keyword::charge, .theme = 0xB07A4AFF},
    {.name = "Огненный бес", .text = "", .cost = 1, .attack = 2, .health = 1, .theme = 0xE0582AFF},
    {.name = "Щитоносец", .text = "Провокация", .cost = 2, .attack = 1, .health = 4, .keywords = Keyword::taunt, .theme = 0x9AA3A8FF},
    {.name = "Жрица рассвета", .text = "Боевой клич: восстановить 3 здоровья.", .cost = 2, .attack = 2, .health = 2,
     .effect = Effect::Heal, .amount = 3, .target = TargetRule::AnyCharacter, .theme = 0xF2D27AFF},
    {.name = "Метатель ножей", .text = "Боевой клич: 1 урона.", .cost = 2, .attack = 2, .health = 1,
     .effect = Effect::Damage, .amount = 1, .target = TargetRule::AnyCharacter, .theme = 0x7E8C5AFF},
    {.name = "Паладин света", .text = "Божественный щит", .cost = 3, .attack = 2, .health = 3,
     .keywords = Keyword::divine_shield, .theme = 0xFFE9A8FF},
    {.name = "Всадник", .text = "Рывок", .cost = 3, .attack = 3, .health = 1, .keywords = Keyword::charge, .theme = 0xA0522DFF},
    {.name = "Призыватель", .text = "Боевой клич: призвать Скелета 1/1.", .cost = 3, .attack = 2, .health = 2,
     .effect = Effect::Summon, .summon = skeleton, .theme = 0x6B5B95FF},
    {.name = "Чародей-наставник", .text = "Боевой клич: возьмите карту.", .cost = 4, .attack = 3, .health = 3,
     .effect = Effect::DrawCards, .amount = 1, .theme = 0x4A7FC1FF},
    {.name = "Каменный страж", .text = "Провокация", .cost = 4, .attack = 3, .health = 5, .keywords = Keyword::taunt,
     .theme = 0x7D7466FF},
    {.name = "Капитан стражи", .text = "Боевой клич: союзному существу +2/+2.", .cost = 5, .attack = 4, .health = 4,
     .effect = Effect::Buff, .amount = 2, .amount2 = 2, .target = TargetRule::FriendlyMinion, .theme = 0x3F6E9EFF},
    {.name = "Огр-громила", .text = "", .cost = 6, .attack = 6, .health = 7, .theme = 0x6E8B3DFF},
    {.name = "Древний дракон", .text = "Боевой клич: 2 урона всем вражеским существам.", .cost = 8, .attack = 8, .health = 8,
     .effect = Effect::DamageEnemyMinions, .amount = 2, .theme = 0xB22222FF},
    {.name = "Скелет", .text = "", .cost = 1, .attack = 1, .health = 1, .theme = 0xD8D2C4FF, .collectible = false},
    // --- заклинания
    {.name = "Искра", .text = "Нанесите 2 урона.", .type = CardType::Spell, .cost = 1, .effect = Effect::Damage, .amount = 2,
     .target = TargetRule::AnyCharacter, .theme = 0xFF8C1AFF},
    {.name = "Огненный шар", .text = "Нанесите 6 урона.", .type = CardType::Spell, .cost = 4, .effect = Effect::Damage,
     .amount = 6, .target = TargetRule::AnyCharacter, .theme = 0xFF4500FF},
    {.name = "Исцеление", .text = "Восстановите 6 здоровья.", .type = CardType::Spell, .cost = 2, .effect = Effect::Heal,
     .amount = 6, .target = TargetRule::AnyCharacter, .theme = 0x7CFC9AFF},
    {.name = "Мудрость", .text = "Возьмите 2 карты.", .type = CardType::Spell, .cost = 3, .effect = Effect::DrawCards,
     .amount = 2, .theme = 0x5FA8D3FF},
    {.name = "Сила предков", .text = "Союзному существу +2/+1.", .type = CardType::Spell, .cost = 1, .effect = Effect::Buff,
     .amount = 2, .amount2 = 1, .target = TargetRule::FriendlyMinion, .theme = 0xC9A227FF},
    {.name = "Волна огня", .text = "2 урона всем вражеским существам.", .type = CardType::Spell, .cost = 4,
     .effect = Effect::DamageEnemyMinions, .amount = 2, .theme = 0xE25822FF},
    {.name = "Землетрясение", .text = "3 урона всем существам.", .type = CardType::Spell, .cost = 5,
     .effect = Effect::DamageAllMinions, .amount = 3, .theme = 0x8B5A2BFF},
    {.name = "Молния", .text = "4 урона вражескому герою.", .type = CardType::Spell, .cost = 3,
     .effect = Effect::DamageEnemyHero, .amount = 4, .theme = 0x9FD3FFFF},
}};

static_assert(library[skeleton].collectible == false);

// =============================================================================
// Генератор: splitmix64 — быстрый, без состояния кроме одного числа, одинаков везде.
// =============================================================================

std::uint64_t next_random(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint32_t random_below(std::uint64_t& state, std::uint32_t bound) noexcept {
    return static_cast<std::uint32_t>(next_random(state) % bound);
}

void record(OutcomeLog* log, const Outcome& outcome) {
    if (log != nullptr) {
        log->push_back(outcome);
    }
}

template<typename E>
std::uint8_t u8(E value) noexcept {
    return static_cast<std::uint8_t>(value);
}

// =============================================================================
// Поиск и изменение
// =============================================================================

Minion* find_minion_mut(Match& match, std::uint32_t uid, int* owner = nullptr) noexcept {
    for (int p = 0; p < 2; ++p) {
        for (Minion& m : match.players[static_cast<std::size_t>(p)].board) {
            if (m.uid == uid) {
                if (owner != nullptr) *owner = p;
                return &m;
            }
        }
    }
    return nullptr;
}

void finish(Match& match, OutcomeLog* log) {
    const bool dead0 = match.players[0].health <= 0;
    const bool dead1 = match.players[1].health <= 0;
    if (!dead0 && !dead1) {
        return;
    }
    match.result = dead0 && dead1 ? Result::Draw : dead0 ? Result::Player1 : Result::Player0;
    record(log, {.kind = u8(OutcomeKind::GameOver), .amount = static_cast<std::int16_t>(match.result)});
}

void deal_damage(Match& match, std::uint32_t target, int amount, OutcomeLog* log) {
    if (amount <= 0) {
        return;
    }
    if (const int hero = hero_owner(target); hero >= 0) {
        Player& p = match.players[static_cast<std::size_t>(hero)];
        p.health = static_cast<std::int16_t>(p.health - amount);
        record(log, {.kind = u8(OutcomeKind::Damage), .amount = static_cast<std::int16_t>(amount), .value = p.health, .uid = target});
        return;
    }
    Minion* m = find_minion_mut(match, target);
    if (m == nullptr || m->health <= 0) {
        return; // уже мертво в этом действии — урон не наносится повторно
    }
    if (m->has(Keyword::divine_shield)) {
        m->keywords = static_cast<std::uint8_t>(m->keywords & ~Keyword::divine_shield);
        record(log, {.kind = u8(OutcomeKind::ShieldPopped), .uid = target});
        return;
    }
    m->health = static_cast<std::int16_t>(m->health - amount);
    record(log, {.kind = u8(OutcomeKind::Damage), .amount = static_cast<std::int16_t>(amount), .value = m->health, .uid = target});
}

void heal(Match& match, std::uint32_t target, int amount, OutcomeLog* log) {
    std::int16_t* health = nullptr;
    std::int16_t max_health = 0;
    if (const int hero = hero_owner(target); hero >= 0) {
        Player& p = match.players[static_cast<std::size_t>(hero)];
        health = &p.health;
        max_health = p.max_health;
    } else if (Minion* m = find_minion_mut(match, target); m != nullptr && m->health > 0) {
        health = &m->health;
        max_health = m->max_health;
    }
    if (health == nullptr) {
        return;
    }
    const auto healed = static_cast<std::int16_t>(std::min<int>(amount, max_health - *health));
    if (healed <= 0) {
        return;
    }
    *health = static_cast<std::int16_t>(*health + healed);
    record(log, {.kind = u8(OutcomeKind::Heal), .amount = healed, .value = *health, .uid = target});
}

/// Убирает погибших: сначала существа активного игрока, затем соперника, по порядку на столе.
void resolve_deaths(Match& match, OutcomeLog* log) {
    for (int k = 0; k < 2; ++k) {
        const int p = k == 0 ? match.active : 1 - match.active;
        auto& board = match.players[static_cast<std::size_t>(p)].board;
        for (std::size_t i = 0; i < board.size();) {
            if (board[i].health <= 0) {
                record(log, {.kind = u8(OutcomeKind::MinionDied), .player = static_cast<std::uint8_t>(p), .card = board[i].card,
                             .uid = board[i].uid});
                board.erase(i);
            } else {
                ++i;
            }
        }
    }
    finish(match, log);
}

void draw_card(Match& match, int player, OutcomeLog* log) {
    Player& p = match.players[static_cast<std::size_t>(player)];
    if (p.deck.empty()) {
        ++p.fatigue;
        record(log, {.kind = u8(OutcomeKind::Fatigue), .player = static_cast<std::uint8_t>(player), .amount = p.fatigue});
        deal_damage(match, hero_uid(player), p.fatigue, log);
        return;
    }
    const CardId id = p.deck.back();
    p.deck.erase(p.deck.size() - 1);
    if (p.hand.full()) {
        record(log, {.kind = u8(OutcomeKind::CardBurned), .player = static_cast<std::uint8_t>(player), .card = id});
        return;
    }
    const std::uint32_t uid = match.next_uid++;
    p.hand.push_back({uid, id});
    record(log, {.kind = u8(OutcomeKind::CardDrawn), .player = static_cast<std::uint8_t>(player), .card = id, .uid = uid});
}

bool summon(Match& match, int player, std::uint32_t uid, CardId id, OutcomeLog* log) {
    Player& p = match.players[static_cast<std::size_t>(player)];
    if (p.board.full()) {
        return false;
    }
    const CardDef& def = card(id);
    const auto slot = static_cast<std::uint8_t>(p.board.size());
    p.board.push_back(Minion{.uid = uid, .card = id, .attack = def.attack, .health = def.health, .max_health = def.health,
                             .keywords = def.keywords, .sleeping = (def.keywords & Keyword::charge) == 0});
    record(log, {.kind = u8(OutcomeKind::MinionSummoned), .player = static_cast<std::uint8_t>(player), .keywords = def.keywords,
                 .slot = slot, .card = id, .amount = def.attack, .value = def.health, .uid = uid});
    return true;
}

/// Эффект карты: `self` — uid сыгранного существа (не задевается «всеми существами»).
void run_effect(Match& match, int player, const CardDef& def, std::uint32_t target, std::uint32_t self, OutcomeLog* log) {
    const int enemy = 1 - player;
    switch (def.effect) {
        case Effect::None: break;
        case Effect::Damage:
            if (target != 0) deal_damage(match, target, def.amount, log);
            break;
        case Effect::Heal:
            if (target != 0) heal(match, target, def.amount, log);
            break;
        case Effect::DamageEnemyMinions: {
            const auto board = match.players[static_cast<std::size_t>(enemy)].board; // копия: урон не меняет состав до resolve
            for (const Minion& m : board) deal_damage(match, m.uid, def.amount, log);
            break;
        }
        case Effect::DamageAllMinions:
            for (int k = 0; k < 2; ++k) {
                const auto board = match.players[static_cast<std::size_t>(k == 0 ? player : enemy)].board;
                for (const Minion& m : board) {
                    if (m.uid != self) deal_damage(match, m.uid, def.amount, log);
                }
            }
            break;
        case Effect::DamageEnemyHero: deal_damage(match, hero_uid(enemy), def.amount, log); break;
        case Effect::DrawCards:
            for (int i = 0; i < def.amount; ++i) draw_card(match, player, log);
            break;
        case Effect::Buff:
            if (Minion* m = find_minion_mut(match, target); m != nullptr) {
                m->attack = static_cast<std::int16_t>(m->attack + def.amount);
                m->health = static_cast<std::int16_t>(m->health + def.amount2);
                m->max_health = static_cast<std::int16_t>(m->max_health + def.amount2);
                record(log, {.kind = u8(OutcomeKind::Buff), .card = m->card, .amount = m->attack, .value = m->health,
                             .value2 = m->max_health, .uid = m->uid});
            }
            break;
        case Effect::Summon: summon(match, player, match.next_uid++, def.summon, log); break;
    }
}

void start_turn(Match& match, OutcomeLog* log) {
    ++match.turn;
    Player& p = match.players[match.active];
    p.max_mana = static_cast<std::uint8_t>(std::min<int>(p.max_mana + 1, max_mana));
    p.mana = p.max_mana;
    for (Minion& m : p.board) {
        m.sleeping = false;
        m.attacked = false;
    }
    record(log, {.kind = u8(OutcomeKind::TurnStarted), .player = match.active, .amount = static_cast<std::int16_t>(match.turn),
                 .value = p.mana, .value2 = p.max_mana});
    if (match.turn > max_turns) {
        match.result = Result::Draw;
        record(log, {.kind = u8(OutcomeKind::GameOver), .amount = static_cast<std::int16_t>(match.result)});
        return;
    }
    draw_card(match, match.active, log);
    finish(match, log);
}

bool contains(const FixedList<std::uint32_t, 16>& list, std::uint32_t uid) noexcept {
    return std::find(list.begin(), list.end(), uid) != list.end();
}

/// Можно ли этим существом атаковать эту цель (провокация учитывается).
bool attack_target_ok(const Match& match, int player, std::uint32_t target) noexcept {
    const Player& enemy = match.players[static_cast<std::size_t>(1 - player)];
    const bool taunts = std::any_of(enemy.board.begin(), enemy.board.end(), [](const Minion& m) { return m.has(Keyword::taunt); });
    if (target == hero_uid(1 - player)) {
        return !taunts;
    }
    for (const Minion& m : enemy.board) {
        if (m.uid == target) return !taunts || m.has(Keyword::taunt);
    }
    return false;
}

} // namespace

// =============================================================================
// Публичное API
// =============================================================================

std::span<const CardDef> card_library() noexcept {
    return library;
}

const CardDef& card(CardId id) noexcept {
    return library[std::min<std::size_t>(id, library.size() - 1)];
}

std::vector<CardId> default_deck(int player) {
    // Игрок 0 — «Свет и сталь»: провокации, лечение, крепкие существа.
    // Игрок 1 — «Огонь и ярость»: рывок, урон, массовые заклинания.
    static constexpr std::array<CardId, 20> light{0, 0, 2, 2, 15, 15, 3, 3, 4, 4, 6, 6, 5, 16, 10, 10, 11, 12, 9, 17};
    static constexpr std::array<CardId, 20> fire{1, 1, 7, 7, 8, 8, 19, 19, 22, 20, 21, 9, 9, 12, 13, 5, 5, 18, 15, 2};
    const auto& deck = player == 0 ? light : fire;
    return {deck.begin(), deck.end()};
}

Match start_match(std::uint64_t seed, std::span<const CardId> deck0, std::span<const CardId> deck1, OutcomeLog* log) {
    Match match{};
    match.rng = seed;
    match.next_uid = 16;
    for (int p = 0; p < 2; ++p) {
        Player& player = match.players[static_cast<std::size_t>(p)];
        player.health = player.max_health = hero_health;
        for (const CardId id : p == 0 ? deck0 : deck1) {
            player.deck.push_back(id);
        }
        // Тасование Фишера–Йетса.
        for (std::size_t i = player.deck.size(); i > 1; --i) {
            std::swap(player.deck[i - 1], player.deck[random_below(match.rng, static_cast<std::uint32_t>(i))]);
        }
    }
    for (int i = 0; i < 3; ++i) draw_card(match, 0, log);
    for (int i = 0; i < 4; ++i) draw_card(match, 1, log);
    match.active = 0;
    start_turn(match, log);
    return match;
}

const Minion* find_minion(const Match& match, std::uint32_t uid, int* owner) noexcept {
    return find_minion_mut(const_cast<Match&>(match), uid, owner);
}

const HandCard* find_hand_card(const Match& match, std::uint32_t uid, int* owner) noexcept {
    for (int p = 0; p < 2; ++p) {
        for (const HandCard& c : match.players[static_cast<std::size_t>(p)].hand) {
            if (c.uid == uid) {
                if (owner != nullptr) *owner = p;
                return &c;
            }
        }
    }
    return nullptr;
}

void valid_targets(const Match& match, int player, TargetRule rule, FixedList<std::uint32_t, 16>& out) noexcept {
    out.clear();
    const int enemy = 1 - player;
    const auto add_hero = [&](int p) { out.push_back(hero_uid(p)); };
    const auto add_board = [&](int p) {
        for (const Minion& m : match.players[static_cast<std::size_t>(p)].board) out.push_back(m.uid);
    };
    switch (rule) {
        case TargetRule::None: break;
        case TargetRule::AnyCharacter:
            add_hero(enemy), add_board(enemy), add_hero(player), add_board(player);
            break;
        case TargetRule::EnemyCharacter: add_hero(enemy), add_board(enemy); break;
        case TargetRule::FriendlyMinion: add_board(player); break;
        case TargetRule::EnemyMinion: add_board(enemy); break;
        case TargetRule::AnyMinion: add_board(enemy), add_board(player); break;
    }
}

bool needs_target(const Match& match, int player, CardId id) noexcept {
    const CardDef& def = card(id);
    if (def.target == TargetRule::None) return false;
    FixedList<std::uint32_t, 16> targets;
    valid_targets(match, player, def.target, targets);
    return !targets.empty();
}

void legal_actions(const Match& match, ActionList& out) noexcept {
    out.clear();
    if (match.over() || match.turn == 0) {
        return;
    }
    const int me = match.active;
    const Player& p = match.players[me];
    FixedList<std::uint32_t, 16> targets;
    for (const HandCard& hc : p.hand) {
        const CardDef& def = card(hc.card);
        if (def.cost > p.mana) continue;
        if (def.type == CardType::Minion && p.board.full()) continue;
        valid_targets(match, me, def.target, targets);
        const Action play{.kind = u8(ActionKind::PlayCard), .player = static_cast<std::uint8_t>(me), .source = hc.uid};
        if (targets.empty()) {
            if (def.type == CardType::Spell && def.target != TargetRule::None) continue; // заклинанию нужна цель
            out.push_back(play);
        } else {
            for (const std::uint32_t t : targets) {
                Action a = play;
                a.target = t;
                out.push_back(a);
            }
        }
    }
    const int enemy = 1 - me;
    for (const Minion& m : p.board) {
        if (!m.can_attack()) continue;
        const Action attack{.kind = u8(ActionKind::Attack), .player = static_cast<std::uint8_t>(me), .source = m.uid};
        if (attack_target_ok(match, me, hero_uid(enemy))) {
            Action a = attack;
            a.target = hero_uid(enemy);
            out.push_back(a);
        }
        for (const Minion& e : match.players[static_cast<std::size_t>(enemy)].board) {
            if (attack_target_ok(match, me, e.uid)) {
                Action a = attack;
                a.target = e.uid;
                out.push_back(a);
            }
        }
    }
    out.push_back({.kind = u8(ActionKind::EndTurn), .player = static_cast<std::uint8_t>(me)});
}

bool is_legal(const Match& match, const Action& action) noexcept {
    if (match.over() || match.turn == 0 || action.player != match.active) {
        return false;
    }
    const int me = match.active;
    const Player& p = match.players[me];
    switch (action.type()) {
        case ActionKind::EndTurn: return true;
        case ActionKind::PlayCard: {
            int owner = -1;
            const HandCard* hc = find_hand_card(match, action.source, &owner);
            if (hc == nullptr || owner != me) return false;
            const CardDef& def = card(hc->card);
            if (def.cost > p.mana) return false;
            if (def.type == CardType::Minion && p.board.full()) return false;
            FixedList<std::uint32_t, 16> targets;
            valid_targets(match, me, def.target, targets);
            if (targets.empty()) {
                return action.target == 0 && !(def.type == CardType::Spell && def.target != TargetRule::None);
            }
            return contains(targets, action.target);
        }
        case ActionKind::Attack: {
            int owner = -1;
            const Minion* m = find_minion(match, action.source, &owner);
            return m != nullptr && owner == me && m->can_attack() && attack_target_ok(match, me, action.target);
        }
        case ActionKind::None: break;
    }
    return false;
}

bool apply(Match& match, const Action& action, OutcomeLog* log) {
    if (!is_legal(match, action)) {
        return false;
    }
    const int me = match.active;
    Player& p = match.players[me];
    switch (action.type()) {
        case ActionKind::EndTurn:
            match.active = static_cast<std::uint8_t>(1 - me);
            start_turn(match, log);
            break;
        case ActionKind::PlayCard: {
            std::size_t index = 0;
            while (p.hand[index].uid != action.source) ++index;
            const HandCard hc = p.hand[index];
            const CardDef& def = card(hc.card);
            p.hand.erase(index);
            p.mana = static_cast<std::uint8_t>(p.mana - def.cost);
            record(log, {.kind = u8(OutcomeKind::CardPlayed), .player = static_cast<std::uint8_t>(me), .card = hc.card,
                         .value = p.mana, .uid = hc.uid, .target = action.target});
            if (def.type == CardType::Minion) {
                summon(match, me, hc.uid, hc.card, log);
                run_effect(match, me, def, action.target, hc.uid, log);
            } else {
                run_effect(match, me, def, action.target, 0, log);
            }
            resolve_deaths(match, log);
            break;
        }
        case ActionKind::Attack: {
            Minion* attacker = find_minion_mut(match, action.source);
            attacker->attacked = true;
            const int damage = attacker->attack;
            record(log, {.kind = u8(OutcomeKind::Attack), .player = static_cast<std::uint8_t>(me), .uid = action.source,
                         .target = action.target});
            const Minion* defender = find_minion(match, action.target);
            const int counter = defender != nullptr ? defender->attack : 0;
            deal_damage(match, action.target, damage, log);
            deal_damage(match, action.source, counter, log);
            resolve_deaths(match, log);
            break;
        }
        case ActionKind::None: return false;
    }
    return true;
}

float evaluate(const Match& match, int player) noexcept {
    const auto winner = player == 0 ? Result::Player0 : Result::Player1;
    if (match.result == winner) return 100000.0f;
    if (match.result == Result::Draw) return 0.0f;
    if (match.over()) return -100000.0f;

    const Player& me = match.players[static_cast<std::size_t>(player)];
    const Player& foe = match.players[static_cast<std::size_t>(1 - player)];
    const auto board_value = [](const Player& p, int& total_attack) {
        float value = 0.0f;
        for (const Minion& m : p.board) {
            value += static_cast<float>(m.attack) * 1.6f + static_cast<float>(m.health) * 1.0f;
            if (m.has(Keyword::taunt)) value += 1.5f;
            if (m.has(Keyword::divine_shield)) value += static_cast<float>(m.attack) * 0.8f + 1.0f;
            total_attack += m.attack;
        }
        return value;
    };
    int my_attack = 0;
    int foe_attack = 0;
    float score = board_value(me, my_attack) - board_value(foe, foe_attack) * 1.15f;
    score += static_cast<float>(me.health) * 1.0f - static_cast<float>(foe.health) * 1.3f;
    score += static_cast<float>(me.hand.size()) * 1.0f - static_cast<float>(foe.hand.size()) * 0.6f;
    if (my_attack >= foe.health) score += 30.0f;   // угроза победы в следующем ходу
    if (foe_attack >= me.health) score -= 40.0f;   // и опасность проиграть
    return score;
}

std::uint64_t hash(const Match& match) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    for (const Player& p : match.players) {
        mix(static_cast<std::uint16_t>(p.health)), mix(static_cast<std::uint16_t>(p.max_health));
        mix(p.mana), mix(p.max_mana), mix(p.fatigue);
        mix(p.deck.size());
        for (const CardId id : p.deck) mix(id);
        mix(p.hand.size());
        for (const HandCard& c : p.hand) mix(c.uid), mix(c.card);
        mix(p.board.size());
        for (const Minion& m : p.board) {
            mix(m.uid), mix(m.card), mix(static_cast<std::uint16_t>(m.attack)), mix(static_cast<std::uint16_t>(m.health));
            mix(static_cast<std::uint16_t>(m.max_health)), mix(m.keywords), mix(m.sleeping), mix(m.attacked);
        }
    }
    mix(match.active), mix(match.turn), mix(match.next_uid), mix(match.rng), mix(static_cast<std::uint8_t>(match.result));
    return h;
}

// =============================================================================
// ИИ
// =============================================================================

AiDecision choose_action(const Match& match, JobSystem::Scheduler& jobs, const AiConfig& config) {
    AiDecision decision;
    ActionList candidates;
    legal_actions(match, candidates);
    decision.candidates = candidates.size();
    if (candidates.empty()) {
        return decision;
    }
    decision.action = candidates[candidates.size() - 1]; // конец хода — всегда последний
    if (candidates.size() == 1) {
        decision.score = evaluate(match, match.active);
        return decision;
    }

    const int me = match.active;
    const auto rollouts = static_cast<std::size_t>(std::max(config.rollouts, 1));
    const std::size_t total = candidates.size() * rollouts;
    std::vector<float> scores(total, 0.0f);
    const std::uint64_t base_seed = hash(match) ^ (config.seed * 0x9E3779B97F4A7C15ULL);

    // Симуляция i: кандидат i / rollouts, продолжение i % rollouts. Пишет только scores[i].
    JobSystem::parallel_for(jobs, total, std::max<std::size_t>(config.grain, 1), [&](std::size_t begin, std::size_t end, std::size_t) {
        MemorySystem::ArenaScope scope(jobs.scratch());
        Match* sim = scope.arena().push<Match>();
        ActionList* options = scope.arena().push<ActionList>();
        for (std::size_t i = begin; i < end; ++i) {
            *sim = match;
            std::uint64_t rng = base_seed ^ (static_cast<std::uint64_t>(i) * 0xD1B54A32D192ED03ULL);
            apply(*sim, candidates[i / rollouts]);
            for (int step = 0; step < config.max_steps && !sim->over() && sim->active == me; ++step) {
                legal_actions(*sim, *options);
                // Случайный план: конец хода с вероятностью ~1/6, иначе любое другое действие.
                const std::size_t others = options->size() - 1;
                const bool stop = others == 0 || random_below(rng, 6) == 0;
                apply(*sim, stop ? (*options)[others] : (*options)[random_below(rng, static_cast<std::uint32_t>(others))]);
            }
            scores[i] = evaluate(*sim, me);
        }
    });

    // Свёртка в фиксированном порядке: лучший план + доля среднего; при равенстве — первый кандидат.
    float best = -std::numeric_limits<float>::infinity();
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        float top = -std::numeric_limits<float>::infinity();
        float sum = 0.0f;
        for (std::size_t r = 0; r < rollouts; ++r) {
            const float s = scores[c * rollouts + r];
            top = std::max(top, s);
            sum += s;
        }
        const float score = top + 0.1f * (sum / static_cast<float>(rollouts));
        if (score > best) {
            best = score;
            decision.action = candidates[c];
        }
    }
    decision.score = best;
    decision.simulations = total;
    return decision;
}

} // namespace CardDuel
