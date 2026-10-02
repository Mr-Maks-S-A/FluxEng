#include <RuntimeSystem/Runtime.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <unordered_map>

namespace RuntimeSystem {

namespace {

using Clock = std::chrono::steady_clock;

std::string_view phase_name(Phase phase) noexcept {
    switch (phase) {
        case Phase::Created: return "Created";
        case Phase::Initializing: return "Initializing";
        case Phase::Running: return "Running";
        case Phase::ShuttingDown: return "ShuttingDown";
        case Phase::Stopped: return "Stopped";
    }
    return "?";
}

} // namespace

Runtime::Runtime(RuntimeConfig config)
    : m_config(config),
      m_jobs(JobSystem::SchedulerConfig{.threads = config.threads >= 0 ? static_cast<unsigned>(config.threads)
                                                                       : JobSystem::default_threads()}),
      m_tick_memory(MemorySystem::DoubleArena::reserve(config.tick_arena_bytes, MemorySystem::KiB(64),
                                                       MemorySystem::MemoryTag::Engine)),
      m_frame_memory(MemorySystem::Arena::reserve(config.frame_arena_bytes, MemorySystem::KiB(64),
                                                  MemorySystem::MemoryTag::Scratch)) {
    if (!(config.ticks_per_second > 0.0)) {
        throw RuntimeError("Runtime: ticks_per_second must be positive");
    }
    m_step.ticks_per_second = config.ticks_per_second;
    m_step.max_ticks_per_frame = std::max(1, config.max_ticks_per_frame);
    m_step.lockstep = config.lockstep;
}

Runtime::~Runtime() {
    shutdown();
    while (!m_modules.empty()) {
        m_modules.pop_back(); // в обратном порядке: зависимые уничтожаются раньше своих зависимостей
    }
}

// ===================================================================== модули

void Runtime::register_module(std::unique_ptr<Module> module) {
    const std::string_view name = module->name();
    if (name.empty()) {
        throw RuntimeError("Runtime::add: module name is empty");
    }
    if (find(name) != nullptr) {
        throw RuntimeError(std::format("Runtime::add: module '{}' is already registered", name));
    }
    m_modules.push_back(Slot{std::move(module), ModuleStats{.name = name}});
}

Module* Runtime::find(std::string_view name) noexcept {
    for (auto& slot : m_modules) {
        if (slot.module->name() == name) return slot.module.get();
    }
    return nullptr;
}

const Module* Runtime::find(std::string_view name) const noexcept {
    return const_cast<Runtime*>(this)->find(name); // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

void Runtime::require_phase(Phase expected, std::string_view action) const {
    if (m_phase != expected) {
        throw RuntimeError(std::format("Runtime::{}: needs phase {}, but it is {}", action, phase_name(expected),
                                       phase_name(m_phase)));
    }
}

// Порядок: Kahn с выбором наименьшего номера регистрации — зависимости раньше, остальное по порядку add().
void Runtime::sort_modules() {
    const std::size_t count = m_modules.size();
    std::unordered_map<std::string_view, std::size_t> index;
    index.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        index.emplace(m_modules[i].module->name(), i);
    }

    std::vector<std::vector<std::size_t>> deps(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (const std::string& dependency : m_modules[i].module->dependencies()) {
            const auto it = index.find(dependency);
            if (it == index.end()) {
                throw RuntimeError(std::format("module '{}' depends on unknown module '{}'", m_modules[i].stats.name, dependency));
            }
            deps[i].push_back(it->second);
        }
    }

    std::vector<bool> placed(count, false);
    std::vector<std::size_t> order;
    order.reserve(count);
    for (std::size_t round = 0; round < count; ++round) {
        std::size_t pick = count;
        for (std::size_t i = 0; i < count && pick == count; ++i) {
            if (!placed[i] && std::ranges::all_of(deps[i], [&](std::size_t d) { return placed[d]; })) pick = i;
        }
        if (pick == count) {
            std::string cycle;
            for (std::size_t i = 0; i < count; ++i) {
                if (!placed[i]) cycle += std::format("{}{}", cycle.empty() ? "" : ", ", m_modules[i].stats.name);
            }
            throw RuntimeError(std::format("module dependency cycle among: {}", cycle));
        }
        placed[pick] = true;
        order.push_back(pick);
    }

    std::vector<Slot> sorted;
    sorted.reserve(count);
    for (const std::size_t i : order) sorted.push_back(std::move(m_modules[i]));
    m_modules = std::move(sorted);
}

std::vector<std::string_view> Runtime::initialization_order() const {
    std::vector<std::string_view> names;
    names.reserve(m_modules.size());
    for (const auto& slot : m_modules) names.push_back(slot.stats.name);
    return names;
}

// ===================================================================== жизненный цикл

void Runtime::initialize() {
    require_phase(Phase::Created, "initialize");
    sort_modules(); // ошибка зависимостей — до любых хуков, фаза остаётся Created

    m_phase = Phase::Initializing;
    m_initialized = 0;
    try {
        for (auto& slot : m_modules) slot.module->declare(*this);
        for (auto& slot : m_modules) {
            slot.module->init(*this);
            ++m_initialized; // shutdown() получат только те, чей init() завершился
        }
    } catch (...) {
        rollback(m_initialized);
        m_phase = Phase::Stopped;
        throw;
    }
    m_phase = Phase::Running;
}

void Runtime::rollback(std::size_t initialized) noexcept {
    for (std::size_t i = initialized; i > 0; --i) {
        m_modules[i - 1].module->shutdown(*this);
    }
    m_initialized = 0;
}

void Runtime::shutdown() noexcept {
    switch (m_phase) {
        case Phase::Created: m_phase = Phase::Stopped; return; // хуки не вызывались — завершать нечего
        case Phase::Running: break;
        default: return; // Initializing: вызов из хука; ShuttingDown/Stopped: уже сделано
    }
    m_phase = Phase::ShuttingDown;
    rollback(m_initialized);
    m_phase = Phase::Stopped;
}

// ===================================================================== время

void Runtime::begin_frame(double frame_seconds) {
    require_phase(Phase::Running, "begin_frame");
    m_frame_memory.reset();
    const auto seconds = static_cast<float>(frame_seconds);
    for (auto& slot : m_modules) slot.module->frame(*this, seconds);
}

int Runtime::update(double frame_seconds) {
    require_phase(Phase::Running, "update");
    m_bus.advance_frame(); // каналы домена Frame живут и на паузе, когда тиков нет
    const int steps = m_step.advance(frame_seconds);
    for (int i = 0; i < steps; ++i) tick();
    return steps;
}

void Runtime::tick_modules() {
    if (!m_config.profile_modules) {
        for (auto& slot : m_modules) slot.module->tick(*this);
        return;
    }
    for (auto& slot : m_modules) {
        const auto start = Clock::now();
        slot.module->tick(*this);
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        ++slot.stats.ticks;
        slot.stats.total_ns += elapsed;
        slot.stats.max_ns = std::max(slot.stats.max_ns, elapsed);
    }
}

void Runtime::tick() {
    require_phase(Phase::Running, "tick");
    tick_modules();
    if (m_tick_callback) m_tick_callback(*this);
    m_bus.advance_tick();
    m_tick_memory.swap(); // память тика N доступна в N+1 как previous, затем очищается
}

int Runtime::run(RunOptions options) {
    if (m_phase == Phase::Created) initialize();
    require_phase(Phase::Running, "run");

    int done = 0;
    const auto finished = [&] { return stop_requested() || (options.max_ticks >= 0 && done >= options.max_ticks); };

    if (!options.realtime) {
        const double dt = m_step.tick_seconds();
        while (!finished()) {
            begin_frame(dt);
            m_bus.advance_frame();
            tick();
            ++done;
        }
        return done;
    }

    auto last = Clock::now();
    while (!finished()) {
        const auto now = Clock::now();
        const double dt = std::min(std::chrono::duration<double>(now - last).count(), 0.25);
        last = now;

        begin_frame(dt);
        m_bus.advance_frame();
        for (int steps = m_step.advance(dt); steps > 0 && !finished(); --steps) {
            tick();
            ++done;
        }

        // Спим до следующего тика: процессор сервера не должен гореть впустую.
        const double speed = static_cast<double>(std::max(1, m_step.speed));
        const double remaining = m_step.paused ? 0.01 : (1.0 - static_cast<double>(m_step.alpha())) * m_step.tick_seconds() / speed;
        if (remaining > 0.0005) {
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.0005));
        }
    }
    return done;
}

std::vector<ModuleStats> Runtime::module_stats() const {
    std::vector<ModuleStats> result;
    if (!m_config.profile_modules) return result;
    result.reserve(m_modules.size());
    for (const auto& slot : m_modules) result.push_back(slot.stats);
    return result;
}

} // namespace RuntimeSystem
