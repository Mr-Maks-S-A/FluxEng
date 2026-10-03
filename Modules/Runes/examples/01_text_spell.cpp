/**
 * @example 01_text_spell.cpp
 * Заклинание из текста: разбор, запуск стековой машины, оплата маной, эффекты как данные, остановка «убегающего» цикла.
 *
 * Мир модуль не знает. Руны читают его через `SpellHost` (чувства и кошелёк маны), а то, что должно измениться
 * в мире, возвращают данными — `Effect` в `EffectBuffer`. Здесь мир заменён крошечной заглушкой.
 */

#include <Runes/Runes.hpp>

#include <cstdio>

using namespace Runes;
using Math::Fixed;
using Math::FVec3;
using Math::Mana;
using Math::WorldPos;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/// «Мир» для примера: маг стоит в точке, прицел всегда попадает в (20, 4, 20), запас маны — число.
struct ToyWorld final : SpellHost {
    Mana personal = Mana::from_int(1000);
    WorldPos target_point = WorldPos::from_meters(20, 4, 20);

    WorldPos position(ECS::Entity) override { return WorldPos::from_meters(10, 5, 10); }
    WorldPos target(ECS::Entity, FVec3, Fixed) override { return target_point; }
    Mana density(WorldPos) override { return Mana::from_int(40); }
    Mana draw(WorldPos, Fixed, Mana amount) override { return amount; } // поле бездонное
    bool take_personal(ECS::Entity, Mana amount) override {
        if (amount > personal) return false;
        personal -= amount;
        return true;
    }
    void give_personal(ECS::Entity, Mana amount) override { personal += amount; }
};

int main() {
    // 1. Заклинание — данные: строка на руну, «#» — комментарий. Проверка при загрузке: известные руны, цели переходов, ≤ 256 рун.
    ProgramLibrary library;
    const auto loaded = library.add_text("carve", "TARGET        # x y z точки прицела\nPUSH 2        # радиус, м\nCARVE\nHALT\n");
    EXPECT(loaded.has_value());
    const auto broken = library.add_text("bad", "FLY\n");
    EXPECT(!broken.has_value());
    std::printf("1. «bad»: код ошибки %.*s — %s\n", static_cast<int>(code_name(broken.error().code).size()), code_name(broken.error().code).data(),
                broken.error().format().c_str());

    // 2. Каст из личного запаса: заклинание — сущность ECS с программой, состоянием машины и остатком бюджета.
    ECS::World world;
    SpellSystem spells;
    ToyWorld toy;
    EffectBuffer effects;
    const ECS::Entity mage = world.create();
    (void)spells.cast(world, mage, library.find("carve"), /*aim*/ {}, ManaSource::Personal);
    spells.tick(world, toy, effects); // исполняет активные заклинания в порядке id; мир не трогает
    std::printf("2. эффектов: %zu; потрачено %.2f маны (4 руны по 0.05 + 30·2³ за эффект); у мага осталось %.2f\n", effects.size(), spells.last_trace().spent.to_double(),
                toy.personal.to_double());
    EXPECT(effects.size() == 1 && effects[0].kind == Effect::Kind::Carve);
    EXPECT(effects[0].position == toy.target_point && effects[0].radius == Fixed::from_int(2));

    // 3. Каст из окружающей маны: платит поле, с мага берётся в 10 раз меньше.
    toy.personal = Mana::from_int(1000);
    effects.clear();
    (void)spells.cast(world, mage, library.find("carve"), {}, ManaSource::Ambient);
    spells.tick(world, toy, effects);
    std::printf("3. то же из окружения: у мага потрачено %.2f (≈ 1/10 от %.2f)\n", 1000.0 - toy.personal.to_double(), spells.last_trace().spent.to_double());
    EXPECT(1000.0 - toy.personal.to_double() < 25.0);

    // 4. Бесконечный цикл: каждая руна стоит маны — заклинание останавливается само, когда платить нечем. Движок не падает.
    EXPECT(library.add_text("runaway", "loop:\n  PUSH 1\n  JMP_IF loop\n").has_value());
    toy.personal = Mana::from_int(5);
    (void)spells.cast(world, mage, library.find("runaway"), {}, ManaSource::Personal);
    int ticks = 0;
    while (spells.active(world) > 0 && ++ticks < 1000) spells.tick(world, toy, effects);
    const SpellTrace& trace = spells.last_trace();
    std::printf("4. «runaway»: остановилось через %d тиков, причина: %.*s, выполнено рун: %u\n", ticks, static_cast<int>(failure_text(trace.failure).size()),
                failure_text(trace.failure).data(), trace.runes_executed);
    EXPECT(trace.failure == Failure::OutOfMana);
    EXPECT(trace.runes_executed > 50 && ticks < 1000);

    // 5. Трасса последнего заклинания — для отладочного оверлея: какие руны исполнены и сколько стоили.
    std::printf("5. трасса: первые руны —");
    for (std::size_t i = 0; i < 3 && i < trace.entries.size(); ++i) std::printf(" [%u %.*s %.2f]", trace.entries[i].pc, static_cast<int>(rune_name(trace.entries[i].rune).size()),
                                                                              rune_name(trace.entries[i].rune).data(), trace.entries[i].cost.to_double());
    std::printf("\nOK\n");
    return 0;
}
