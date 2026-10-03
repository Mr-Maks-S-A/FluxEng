/**
 * @file main.cpp
 * @brief WorldJournal — эксперимент: мир как «сид + журнал правок» вместо дампа всех чанков.
 *
 * Идея из ТЗ пре-альфы: «мир пересоздаётся из сида и записи команд». Ландшафт — 25 МБ данных, но почти всё в нём
 * воспроизводится из сида, а изменения игрока — это короткие правки (сфера, радиус, точка). Значит, сохранение — это
 * журнал правок (EventLog: с избыточностью и проверкой целостности), а загрузка — генерация из сида и повтор правок.
 *
 * Эксперимент проверяет три способа сохранить один и тот же мир и сравнивает их:
 *   1. **полный дамп** — все 256 чанков (эталон «как не надо»);
 *   2. **только журнал** — сид + все правки с начала игры;
 *   3. **снимок + хвост журнала** — изменённые чанки на момент контрольной точки + правки после неё.
 * И что бывает при повреждении файла: журнал выдерживает порчу блоков (код Рида—Соломона), а при потере целой полосы
 * снимок делает потерю в начале журнала безвредной.
 *
 * Использование: `WorldJournal [--seed N] [--edits M] [--snapshot-every K] [--scatter] [--dir каталог]`.
 * По умолчанию правки идут на нескольких участках (как в игре); `--scatter` разбрасывает их по всему миру — тогда снимок
 * затрагивает почти все чанки и перестаёт выигрывать у дампа (это и есть вывод эксперимента). Код выхода 0 — все проверки сошлись.
 */

#include <EventLog/Journal.hpp>
#include <Math/Rng.hpp>
#include <Terrain/Terrain.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using Math::Fixed;
using Math::WorldPos;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

/// Правка мира как событие журнала: плоская структура, без указателей — пишется байтами.
struct WorldEdit {
    std::int32_t kind = 0;       ///< 0 — вырезать сферу, 1 — насыпать.
    std::int32_t radius_raw = 0; ///< Радиус, Fixed.raw.
    std::int64_t x = 0, y = 0, z = 0; ///< Центр, WorldPos.
    static constexpr std::string_view event_name = "world.edit";
};
static_assert(sizeof(WorldEdit) == 32 && std::is_trivially_copyable_v<WorldEdit>);

constexpr std::uint32_t snapshot_meta = 1, snapshot_chunk = 2; // типы записей снимка
const EventLog::Config journal_config{.data_blocks = 8, .parity_blocks = 2, .block_size = 4096};

void apply(Terrain::SdfWorld& world, const WorldEdit& e) {
    const WorldPos at{e.x, e.y, e.z};
    const Fixed radius = Fixed::from_raw(e.radius_raw);
    if (e.kind == 0) (void)world.carve_sphere(at, radius);
    else (void)world.add_sphere(at, radius);
}

/// Детерминированный поток правок. По умолчанию «игрок» работает на нескольких участках (как в настоящей игре: стройка, шахта,
/// воронка от заклинаний), поэтому затрагивается малая часть мира; `scatter` разбрасывает правки по всему миру (худший случай).
struct EditSource {
    Math::Rng rng;
    bool scatter;
    std::array<std::pair<std::int64_t, std::int64_t>, 6> sites{}; ///< Центры участков (x, z), WorldPos.

    EditSource(std::uint64_t seed, bool scatter_all) : rng(seed ^ 0xED17), scatter(scatter_all) {
        for (auto& site : sites) site = {static_cast<std::int64_t>(20 + rng.below(88)) * 65536, static_cast<std::int64_t>(20 + rng.below(88)) * 65536};
    }

    WorldEdit next(const Terrain::SdfWorld& world) {
        WorldEdit e;
        e.kind = rng.below(10) < 7 ? 0 : 1;
        e.radius_raw = static_cast<std::int32_t>((1 + rng.below(4)) * 65536 + rng.below(65536));
        if (scatter) {
            e.x = static_cast<std::int64_t>(rng.below(127)) * 65536 + rng.below(65536);
            e.z = static_cast<std::int64_t>(rng.below(127)) * 65536 + rng.below(65536);
            e.y = static_cast<std::int64_t>(5 + rng.below(45)) * 65536 + rng.below(65536);
        } else {
            const auto& site = sites[rng.below(static_cast<std::uint32_t>(sites.size()))];
            e.x = site.first + (static_cast<std::int64_t>(rng.below(24)) - 12) * 65536;
            e.z = site.second + (static_cast<std::int64_t>(rng.below(24)) - 12) * 65536;
            e.y = world.ground_height(e.x, e.z) - static_cast<std::int64_t>(rng.below(6)) * 65536; // у поверхности: копают и насыпают рядом с землёй
        }
        return e;
    }
};

// ---- Снимок: изменённые чанки (остальные воспроизводит сид). Тоже журнал EventLog: с проверкой целостности и избыточностью. ----

std::size_t write_snapshot(const Terrain::SdfWorld& world, const fs::path& path, std::uint64_t edits_included) {
    auto storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Create);
    auto writer = EventLog::Writer::create(*storage, journal_config).value();
    std::byte meta[8];
    std::memcpy(meta, &edits_included, 8);
    writer.append(0, snapshot_meta, meta);
    std::size_t chunks = 0;
    std::vector<std::byte> payload(4 + sizeof(Terrain::Chunk::distance) + sizeof(Terrain::Chunk::material));
    for (int i = 0; i < world.layout().chunk_count(); ++i) {
        const Terrain::ChunkCoord c = world.layout().coord(i);
        if (!world.modified(c)) continue;
        const auto index = static_cast<std::uint32_t>(i);
        std::memcpy(payload.data(), &index, 4);
        std::memcpy(payload.data() + 4, world.chunk(c).distance.data(), sizeof(Terrain::Chunk::distance));
        std::memcpy(payload.data() + 4 + sizeof(Terrain::Chunk::distance), world.chunk(c).material.data(), sizeof(Terrain::Chunk::material));
        writer.append(0, snapshot_chunk, payload);
        ++chunks;
    }
    (void)writer.flush();
    return chunks;
}

struct SnapshotResult {
    bool ok = false;
    std::uint64_t edits_included = 0;
    std::size_t chunks = 0;
    EventLog::RecoveryReport report;
};

SnapshotResult load_snapshot(Terrain::SdfWorld& world, const fs::path& path) {
    SnapshotResult result;
    auto storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Existing);
    if (!storage) return result;
    auto read = EventLog::read_all(*storage);
    if (!read) return result;
    result.report = read->report;
    bool have_meta = false;
    for (const EventLog::Record& r : read->records) {
        if (r.type == snapshot_meta && r.payload.size() == 8) {
            std::memcpy(&result.edits_included, r.payload.data(), 8);
            have_meta = true;
        } else if (r.type == snapshot_chunk && r.payload.size() == 4 + sizeof(Terrain::Chunk::distance) + sizeof(Terrain::Chunk::material)) {
            std::uint32_t index = 0;
            std::memcpy(&index, r.payload.data(), 4);
            if (index >= static_cast<std::uint32_t>(world.layout().chunk_count())) continue;
            Terrain::Chunk chunk;
            std::memcpy(chunk.distance.data(), r.payload.data() + 4, sizeof(chunk.distance));
            std::memcpy(chunk.material.data(), r.payload.data() + 4 + sizeof(chunk.distance), sizeof(chunk.material));
            world.load_chunk(world.layout().coord(static_cast<int>(index)), chunk);
            ++result.chunks;
        }
    }
    result.ok = have_meta;
    return result;
}

/// Загрузка из сида и журнала правок; `from_sequence` — с какой правки повторять (после снимка — с его номера).
struct JournalLoad {
    std::uint64_t applied = 0;
    EventLog::RecoveryReport report;
};

JournalLoad replay_journal(Terrain::SdfWorld& world, const fs::path& path, std::uint64_t from_sequence) {
    JournalLoad result;
    auto storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Existing);
    auto read = EventLog::read_all(*storage);
    if (!read) return result;
    result.report = read->report;
    for (const EventLog::Record& r : read->records) {
        if (r.sequence < from_sequence) continue;
        if (const auto e = EventLog::decode<WorldEdit>(r)) {
            apply(world, *e);
            ++result.applied;
        }
    }
    return result;
}

std::string kib(std::uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024 * 1024) std::snprintf(buffer, sizeof buffer, "%.1f МиБ", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(buffer, sizeof buffer, "%.1f КиБ", static_cast<double>(bytes) / 1024.0);
    return buffer;
}

int g_failed = 0;
void expect(bool condition, const char* what) {
    std::printf("   [%s] %s\n", condition ? "ок" : "ОШИБКА", what);
    if (!condition) ++g_failed;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 7, edits = 4000, snapshot_every = 1500;
    bool scatter = false;
    fs::path dir = fs::temp_directory_path() / "world_journal_demo";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scatter") scatter = true;
        if (i + 1 >= argc) break;
        if (a == "--seed") seed = std::strtoull(argv[i + 1], nullptr, 10);
        if (a == "--edits") edits = std::strtoull(argv[i + 1], nullptr, 10);
        if (a == "--snapshot-every") snapshot_every = std::strtoull(argv[i + 1], nullptr, 10);
        if (a == "--dir") dir = argv[i + 1];
    }
    fs::create_directories(dir);
    const fs::path journal_path = dir / "world.journal", snapshot_path = dir / "world.snapshot";
    std::printf("WorldJournal: сид %llu, правок %llu (%s), контрольная точка каждые %llu\n\n", static_cast<unsigned long long>(seed), static_cast<unsigned long long>(edits),
                scatter ? "разбросаны по всему миру" : "на 6 участках", static_cast<unsigned long long>(snapshot_every));

    // ===== 1. «Игра»: правки применяются к миру и пишутся в журнал; раз в K правок — снимок изменённых чанков =====
    Terrain::SdfWorld world(seed);
    std::uint64_t snapshot_edits = 0;
    std::size_t snapshot_chunks = 0;
    {
        auto storage = EventLog::FileStorage::open(journal_path, EventLog::FileStorage::Mode::Create);
        auto writer = EventLog::Writer::create(*storage, journal_config).value();
        EditSource source(seed, scatter);
        const auto t0 = Clock::now();
        for (std::uint64_t i = 0; i < edits; ++i) {
            const WorldEdit e = source.next(world);
            writer.append(static_cast<std::uint32_t>(i), e); // сначала в журнал, потом в мир: журнал не отстаёт от мира
            apply(world, e);
            if (snapshot_every > 0 && (i + 1) % snapshot_every == 0) {
                snapshot_chunks = write_snapshot(world, snapshot_path, i + 1);
                snapshot_edits = i + 1;
            }
        }
        (void)writer.flush();
        std::printf("1. игра: %llu правок за %.0f мс (с записью журнала и снимков)\n", static_cast<unsigned long long>(edits), ms_since(t0));
    }
    const std::uint64_t saved_hash = world.hash();
    int modified = 0;
    for (int i = 0; i < world.layout().chunk_count(); ++i) modified += world.modified(world.layout().coord(i)) ? 1 : 0;

    // ===== 2. Сколько места =====
    const std::uint64_t full_dump = static_cast<std::uint64_t>(world.layout().chunk_count()) * sizeof(Terrain::Chunk);
    const std::uint64_t journal_size = fs::file_size(journal_path);
    const std::uint64_t snapshot_size = snapshot_edits > 0 ? fs::file_size(snapshot_path) : 0;
    std::printf("\n2. размер сохранения:\n   полный дамп всех чанков   %12s\n   журнал правок (+чётность) %12s   (%.1f%% от дампа)\n", kib(full_dump).c_str(), kib(journal_size).c_str(),
                100.0 * static_cast<double>(journal_size) / static_cast<double>(full_dump));
    if (snapshot_edits > 0) {
        std::printf("   снимок (%zu из %d чанков)  %12s   (контрольная точка после %llu правок)\n", snapshot_chunks, world.layout().chunk_count(), kib(snapshot_size).c_str(),
                    static_cast<unsigned long long>(snapshot_edits));
    }
    std::printf("   изменённых чанков в мире: %d из %d — остальные воспроизводит сид\n", modified, world.layout().chunk_count());
    if (snapshot_edits > 0 && snapshot_size > full_dump)
        std::printf("   ВЫВОД: правки разбросаны по всему миру — снимок (с чётностью) больше дампа. Снимок выигрывает только при локальных правках;\n"
                    "          иначе достаточно журнала.\n");

    // ===== 3. Загрузка: журнал целиком и снимок + хвост =====
    std::printf("\n3. загрузка:\n");
    {
        Terrain::SdfWorld loaded(seed);
        const auto t0 = Clock::now();
        const JournalLoad j = replay_journal(loaded, journal_path, 0);
        const double ms = ms_since(t0);
        std::printf("   только журнал: сид + %llu правок за %.0f мс\n", static_cast<unsigned long long>(j.applied), ms);
        expect(loaded.hash() == saved_hash, "мир из журнала побитово совпал с исходным (хеш)");
    }
    if (snapshot_edits > 0) {
        Terrain::SdfWorld loaded(seed);
        const auto t0 = Clock::now();
        const SnapshotResult snap = load_snapshot(loaded, snapshot_path);
        const JournalLoad tail = replay_journal(loaded, journal_path, snap.edits_included);
        const double ms = ms_since(t0);
        std::printf("   снимок + хвост: %zu чанков из снимка + %llu правок после него за %.0f мс\n", snap.chunks, static_cast<unsigned long long>(tail.applied), ms);
        expect(snap.ok && loaded.hash() == saved_hash, "мир из снимка и хвоста журнала побитово совпал с исходным (хеш)");
    }

    // ===== 4. Повреждения файла =====
    std::printf("\n4. повреждения журнала:\n");
    const std::uint64_t stripes = EventLog::verify(*EventLog::FileStorage::open(journal_path, EventLog::FileStorage::Mode::Existing))->stripes;
    const auto corrupt = [&](std::uint64_t stripe, std::initializer_list<int> blocks) {
        auto storage = EventLog::FileStorage::open(journal_path, EventLog::FileStorage::Mode::Existing);
        for (const int b : blocks) {
            const std::uint64_t at = EventLog::block_offset(journal_config, stripe, b) + 100;
            std::byte byte{};
            (void)storage->read(at, std::span<std::byte>(&byte, 1));
            byte ^= std::byte{0xFF};
            (void)storage->write(at, std::span<const std::byte>(&byte, 1));
        }
    };
    // 4а. В каждой полосе испорчено до двух блоков (m = 2): журнал восстанавливается полностью.
    for (std::uint64_t s = 0; s < stripes; ++s) corrupt(s, {static_cast<int>(s % 8), 9});
    {
        Terrain::SdfWorld loaded(seed);
        const JournalLoad j = replay_journal(loaded, journal_path, 0);
        std::printf("   а) по два испорченных блока в каждой из %llu полос: восстановлено блоков %llu, правок применено %llu\n", static_cast<unsigned long long>(stripes),
                    static_cast<unsigned long long>(j.report.blocks_repaired), static_cast<unsigned long long>(j.applied));
        expect(loaded.hash() == saved_hash && j.report.blocks_lost == 0, "мир восстановлен без потерь");
    }
    // 4б. Потеряна целая первая полоса (три блока при m = 2): первые правки журнала пропали.
    corrupt(0, {0, 1, 2});
    {
        Terrain::SdfWorld loaded(seed);
        const JournalLoad j = replay_journal(loaded, journal_path, 0);
        std::printf("   б) разрушена первая полоса целиком: потеряно правок %llu (пропусков %zu), применено %llu из %llu\n",
                    static_cast<unsigned long long>(j.report.records_lost), j.report.gaps.size(), static_cast<unsigned long long>(j.applied), static_cast<unsigned long long>(edits));
        expect(j.report.records_lost > 0 && loaded.hash() != saved_hash, "журнал честно сообщает о потере, мир без этих правок отличается (подмены нет)");
        if (snapshot_edits > 0 && j.report.gaps.size() == 1 && j.report.gaps.front().last_sequence < snapshot_edits) {
            Terrain::SdfWorld from_snapshot(seed);
            const SnapshotResult snap = load_snapshot(from_snapshot, snapshot_path);
            const JournalLoad tail = replay_journal(from_snapshot, journal_path, snap.edits_included);
            std::printf("      потерянные правки №%llu…№%llu все раньше контрольной точки (после %llu правок) — снимок делает потерю безвредной\n",
                        static_cast<unsigned long long>(j.report.gaps.front().first_sequence), static_cast<unsigned long long>(j.report.gaps.front().last_sequence),
                        static_cast<unsigned long long>(snap.edits_included));
            (void)tail;
            expect(from_snapshot.hash() == saved_hash, "снимок + хвост восстановили мир полностью, несмотря на потерю в начале журнала");
        }
    }
    std::printf("\n%s\n", g_failed == 0 ? "OK" : "ОШИБКА: не все проверки сошлись");
    std::error_code ec;
    fs::remove_all(dir, ec);
    return g_failed == 0 ? 0 : 1;
}
