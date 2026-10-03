#include <RuneEditor/Autosave.hpp>

#include <Runes/Graph.hpp>

namespace RuneEditor {

namespace {
constexpr std::string_view snapshot_name = "rune_editor.snapshot";
std::uint32_t snapshot_type() { return EventLog::type_id(snapshot_name); }
} // namespace

Autosave::Autosave(EventLog::Storage& storage, Editor& editor, EventLog::Writer writer, EventLog::Config config)
    : m_storage(&storage), m_editor(&editor), m_writer(std::make_unique<EventLog::Writer>(std::move(writer))), m_config(config) {}

Autosave::~Autosave() { detach(); }

void Autosave::write_snapshot() {
    const std::string text = Runes::serialize(m_editor->graph());
    m_writer->append(0, snapshot_type(), std::as_bytes(std::span(text)));
}

void Autosave::attach() {
    // Счётчик живёт в куче: Autosave можно переместить, наблюдатель не потеряет адрес.
    EventLog::Writer* writer = m_writer.get();
    std::uint64_t* ops = m_ops.get();
    m_editor->set_observer([writer, ops](const Op& op) {
        writer->append(0, op);
        ++*ops;
    });
}

std::expected<Autosave, std::string> Autosave::start(EventLog::Storage& storage, Editor& editor, const EventLog::Config& config) {
    auto writer = EventLog::Writer::create(storage, config);
    if (!writer) return std::unexpected(writer.error());
    Autosave a(storage, editor, std::move(*writer), config);
    a.write_snapshot();
    a.attach();
    if (!a.flush()) return std::unexpected("не удалось записать журнал автосохранения");
    return a;
}

bool Autosave::flush() { return m_writer->flush(); }

bool Autosave::compact() {
    m_editor->set_observer(nullptr);
    auto writer = EventLog::Writer::create(*m_storage, m_config);
    if (!writer) return false;
    *m_writer = std::move(*writer);
    write_snapshot();
    attach();
    return m_writer->flush();
}

std::expected<Autosave::Recovered, std::string> Autosave::recover(EventLog::Storage& storage) {
    auto read = EventLog::read_all(storage, true);
    if (!read) return std::unexpected(read.error());
    Recovered out;
    out.report = read->report;

    // Последний снимок, который удаётся разобрать.
    std::size_t start = read->records.size();
    for (std::size_t i = read->records.size(); i-- > 0;) {
        const EventLog::Record& r = read->records[i];
        if (r.type != snapshot_type()) continue;
        ++out.snapshots;
        if (start != read->records.size()) continue;
        const std::string text(reinterpret_cast<const char*>(r.payload.data()), r.payload.size());
        if (auto g = Runes::parse_graph(text)) {
            out.graph = std::move(*g);
            start = i;
        }
    }
    if (start == read->records.size()) return std::unexpected("в журнале нет ни одного читаемого снимка");

    Editor replay(out.graph);
    for (std::size_t i = start + 1; i < read->records.size(); ++i) {
        if (const auto op = EventLog::decode<Op>(read->records[i])) {
            replay.execute(*op);
            ++out.operations;
        }
    }
    out.graph = replay.graph();
    return out;
}

} // namespace RuneEditor
