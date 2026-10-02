#include <MemorySystem/Tags.hpp>

#include <array>
#include <atomic>
#include <format>

namespace MemorySystem {

namespace {

struct Counters {
    std::atomic<std::size_t> committed{0};
    std::atomic<std::size_t> peak{0};
    std::atomic<std::size_t> regions{0};
};

std::array<Counters, static_cast<std::size_t>(MemoryTag::Count)>& counters() noexcept {
    static std::array<Counters, static_cast<std::size_t>(MemoryTag::Count)> table;
    return table;
}

Counters& of(MemoryTag tag) noexcept {
    return counters()[static_cast<std::size_t>(tag) < counters().size() ? static_cast<std::size_t>(tag) : 0];
}

std::string human(std::size_t bytes) {
    if (bytes >= std::size_t{1} << 20) return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= std::size_t{1} << 10) return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    return std::format("{} B", bytes);
}

} // namespace

std::string_view to_string(MemoryTag tag) noexcept {
    switch (tag) {
        case MemoryTag::Untagged: return "untagged";
        case MemoryTag::Engine: return "engine";
        case MemoryTag::Events: return "events";
        case MemoryTag::ECS: return "ecs";
        case MemoryTag::Jobs: return "jobs";
        case MemoryTag::Renderer: return "renderer";
        case MemoryTag::Game: return "game";
        case MemoryTag::Scratch: return "scratch";
        case MemoryTag::Count: break;
    }
    return "?";
}

TagStats tag_stats(MemoryTag tag) noexcept {
    const Counters& c = of(tag);
    return {c.committed.load(std::memory_order_relaxed), c.peak.load(std::memory_order_relaxed), c.regions.load(std::memory_order_relaxed)};
}

std::string memory_report() {
    std::string out = std::format("{:<10} {:>12} {:>12} {:>7}\n", "tag", "committed", "peak", "arenas");
    std::size_t total = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(MemoryTag::Count); ++i) {
        const TagStats s = tag_stats(static_cast<MemoryTag>(i));
        if (s.peak == 0 && s.regions == 0) continue;
        out += std::format("{:<10} {:>12} {:>12} {:>7}\n", to_string(static_cast<MemoryTag>(i)), human(s.committed), human(s.peak), s.regions);
        total += s.committed;
    }
    out += std::format("{:<10} {:>12}\n", "total", human(total));
    return out;
}

namespace detail {

void track_commit(MemoryTag tag, std::size_t bytes) noexcept {
    Counters& c = of(tag);
    const std::size_t now = c.committed.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    std::size_t peak = c.peak.load(std::memory_order_relaxed);
    while (now > peak && !c.peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
}

void track_decommit(MemoryTag tag, std::size_t bytes) noexcept {
    of(tag).committed.fetch_sub(bytes, std::memory_order_relaxed);
}

void track_region(MemoryTag tag, int delta) noexcept {
    if (delta > 0) {
        of(tag).regions.fetch_add(1, std::memory_order_relaxed);
    } else {
        of(tag).regions.fetch_sub(1, std::memory_order_relaxed);
    }
}

} // namespace detail
} // namespace MemorySystem
