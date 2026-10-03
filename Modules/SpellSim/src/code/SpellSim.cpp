#include <SpellSim/SpellSim.hpp>

#include <Math/Hash.hpp>

namespace SpellSim {

using Math::Fixed;
using Math::Mana;
using Math::FVec3;
using Math::WorldPos;

Replay::Command MoveCommand::encode() const noexcept {
    return {.type = static_cast<std::uint16_t>(CommandType::Move), .arg = 0, .x = dx.raw, .y = 0, .z = dz.raw};
}
MoveCommand MoveCommand::decode(const Replay::Command& c) noexcept { return {Fixed::from_raw(c.x), Fixed::from_raw(c.z)}; }

Replay::Command JumpCommand::encode() const noexcept { return {.type = static_cast<std::uint16_t>(CommandType::Jump)}; }

Replay::Command CastCommand::encode() const noexcept {
    return {.type = static_cast<std::uint16_t>(CommandType::Cast),
            .arg = static_cast<std::int16_t>(slot | (static_cast<int>(source) << 8)),
            .x = aim.x.raw, .y = aim.y.raw, .z = aim.z.raw};
}
CastCommand CastCommand::decode(const Replay::Command& c) noexcept {
    return {c.arg & 0xFF, static_cast<Runes::ManaSource>((c.arg >> 8) & 0xFF), {Fixed::from_raw(c.x), Fixed::from_raw(c.y), Fixed::from_raw(c.z)}};
}

void register_commands(Replay::CommandRegistry& registry) {
    using Field = Replay::CommandField;
    constexpr auto fx = Field::Kind::Fixed;
    registry.add({static_cast<std::uint16_t>(CommandType::Move), "move", {Field{}, Field{"dx", fx}, Field{}, Field{"dz", fx}}})
        .add({static_cast<std::uint16_t>(CommandType::Jump), "jump", {}})
        .add({static_cast<std::uint16_t>(CommandType::Cast), "cast", {Field{"slot_source", Field::Kind::Int}, Field{"aim_x", fx}, Field{"aim_y", fx}, Field{"aim_z", fx}}});
}

/// Мир глазами заклинаний: чувства и кошелёк. Эффекты на ландшафт руны возвращают данными (`EffectBuffer`), фаза 3 их применяет.
class Simulation::Host final : public Runes::SpellHost {
public:
    explicit Host(Simulation& sim) : m_sim(sim) {}

    WorldPos position(ECS::Entity caster) override { return Character::eye(m_sim.m_world, caster); }
    WorldPos target(ECS::Entity caster, FVec3 aim, Fixed range) override {
        const WorldPos from = position(caster);
        if (const auto hit = m_sim.m_terrain.raycast(from, Math::normalize(aim), range)) return hit->position;
        return Math::advance(from, Math::normalize(aim), range);
    }
    Mana density(WorldPos pos) override { return m_sim.m_mana.density(pos); }
    Mana draw(WorldPos pos, Fixed radius, Mana amount) override { return m_sim.m_mana.draw(pos, radius, amount); }
    bool take_personal(ECS::Entity caster, Mana amount) override {
        Character::ManaPool* pool = m_sim.m_world.get<Character::ManaPool>(caster);
        if (!pool || amount > pool->current) return false;
        pool->current -= amount;
        return true;
    }
    void give_personal(ECS::Entity caster, Mana amount) override {
        if (Character::ManaPool* pool = m_sim.m_world.get<Character::ManaPool>(caster)) pool->current = Math::min(pool->max, pool->current + amount);
    }

private:
    Simulation& m_sim;
};

Simulation::Simulation(const Config& config)
    : m_config(config), m_terrain(config.seed), m_mana(config.mana), m_spells(config.runes), m_rng(config.seed), m_host(std::make_unique<Host>(*this)) {
    build_schedule();
    Character::Grimoire grimoire;
    grimoire.slots = config.grimoire;
    const std::int64_t cx = m_terrain.layout().size_x() / 2, cz = m_terrain.layout().size_z() / 2;
    const WorldPos feet{cx, m_terrain.ground_height(cx, cz) + Fixed::from_ratio(1, 2).raw, cz};
    m_player = Character::spawn(m_world, feet, {config.player_mana, config.player_mana, config.player_mana_regen}, grimoire);
}

void Simulation::declare(EventSystem::EventBus& bus) {
    m_spells.declare(bus);
    const EventSystem::ModuleId id =
        bus.declare_module("SpellSim").produces<TerrainEditedEvent>(EventSystem::ChannelConfig{.reserve = 16, .max_events_per_tick = 256});
    m_terrain_edited = bus.writer<TerrainEditedEvent>(id);
    m_declared = true;
}

void Simulation::set_grimoire_slot(int slot, std::string name) {
    if (slot < 0 || slot >= Character::Grimoire::slot_count) return;
    m_world.get<Character::Grimoire>(m_player)->slots[static_cast<std::size_t>(slot)] = std::move(name);
}

Runes::ProgramLibrary::Report Simulation::reload_spells(const std::filesystem::path& directory) { return m_programs.load_directory(directory); }

void Simulation::apply(const Replay::Command& c) {
    switch (static_cast<CommandType>(c.type)) {
    case CommandType::Move: {
        const MoveCommand move = MoveCommand::decode(c);
        FVec3 dir{move.dx, Fixed{}, move.dz};
        if (Math::length(dir) > Fixed::from_int(1)) dir = Math::normalize(dir);
        m_world.get<Character::Motor>(m_player)->wish = dir;
        break;
    }
    case CommandType::Jump: m_world.get<Character::Motor>(m_player)->jump = true; break;
    case CommandType::Cast: {
        const CastCommand cast = CastCommand::decode(c);
        if (cast.slot < 0 || cast.slot >= Character::Grimoire::slot_count) break;
        const std::string& name = m_world.get<Character::Grimoire>(m_player)->slots[static_cast<std::size_t>(cast.slot)];
        std::shared_ptr<const Runes::Program> program = m_programs.find(name);
        if (!program) {
            ++m_failed_casts; // в слоте нет заклинания (файл не найден или с ошибкой)
            break;
        }
        (void)m_spells.cast(m_world, m_player, std::move(program), Math::normalize(cast.aim), cast.source);
        ++m_casts;
        break;
    }
    }
}

void Simulation::build_schedule() {
    // Порядок фаз фиксирован и виден: это список, а не вызовы в одной функции. Новые модули (машины, гравитация тел)
    // подключаются через `schedule().insert_after(...)`.
    m_schedule.add("commands", [this] { phase_commands(); })
        .add("spells", [this] { phase_spells(); })
        .add("terrain", [this] { phase_terrain(); })
        .add("mana", [this] { phase_mana(); })
        .add("movement", [this] { phase_movement(); })
        .add("events", [this] { phase_events(); });
}

// 1. команды игрока
void Simulation::phase_commands() {
    for (const Replay::Command& c : m_commands) apply(c);
}

// 2. активные заклинания в порядке id; мир они меняют только данными (эффектами)
void Simulation::phase_spells() { m_spells.tick(m_world, *m_host, m_effects); }

// 3. правки ландшафта — эффекты в порядке исполнения
void Simulation::phase_terrain() {
    m_edited.clear();
    for (const Runes::Effect& e : m_effects) {
        Terrain::EditResult r = e.kind == Runes::Effect::Kind::Carve ? m_terrain.carve_sphere(e.position, e.radius) : m_terrain.add_sphere(e.position, e.radius);
        if (r.changed()) m_edited.push_back(std::move(r));
        ++m_edits_applied;
    }
    m_effects.clear();
}

// 4. поле маны: каждый шестой тик
void Simulation::phase_mana() {
    if (m_tick % mana_step_period == 0) m_mana.step();
}

// 5. движение и столкновения
void Simulation::phase_movement() { Character::step(m_world, m_terrain, m_config.character); }

// 6. события; изменённые чанки уже стоят в очереди SdfWorld::take_dirty()
void Simulation::phase_events() {
    if (!m_declared) return;
    for (const Terrain::EditResult& r : m_edited) {
        (void)m_terrain_edited.emit(TerrainEditedEvent{r.bounds.lo[0], r.bounds.lo[1], r.bounds.lo[2], r.bounds.hi[0], r.bounds.hi[1], r.bounds.hi[2],
                                                       static_cast<std::int32_t>(r.chunks.size())});
    }
}

void Simulation::tick(std::span<const Replay::Command> commands) {
    m_commands = commands;
    m_schedule.run();
    m_commands = {};
    ++m_tick;
}

Replay::StateHashes Simulation::hashes() const {
    // Хеши названы по подсистемам: при расхождении повтора сразу видно, какая подсистема разошлась.
    Math::Hasher characters, spells, rng, tick;
    Character::hash_characters(m_world, characters);
    Runes::hash_spells(m_world, spells);
    rng.add(m_rng.state());
    tick.add(m_tick);
    Replay::StateHashes h;
    h.add("terrain", m_terrain.hash()).add("mana", m_mana.hash()).add("characters", characters.value()).add("spells", spells.value()).add("rng", rng.value()).add("tick", tick.value());
    return h;
}

} // namespace SpellSim
