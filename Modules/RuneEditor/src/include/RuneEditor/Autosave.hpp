#pragma once
/**
 * @file Autosave.hpp
 * @brief Автосохранение редактора в журнал EventLog: снимок графа + правки после него. Журнал переживает обрыв и частичную порчу.
 *
 * ```
 *   [снимок] [op] [op] [op] … [снимок] [op] …        ← записи журнала (Рид—Соломон по полосам)
 * ```
 * Восстановление: берётся **последний** снимок и к нему применяются правки, записанные после (через тот же
 * `Editor::execute`, поэтому undo/redo восстанавливаются тоже). Если последние полосы потеряны — получится состояние на
 * последнюю устойчивую точку (`flush`), а не пустой редактор. `compact` пишет журнал заново с одним снимком.
 */

#include <RuneEditor/Editor.hpp>

#include <EventLog/Journal.hpp>

#include <optional>
#include <utility>

namespace RuneEditor {

class Autosave {
public:
    /// @brief Начинает новый журнал в `storage` (содержимое стирается) и пишет снимок текущего графа редактора. Подписывается на правки.
    [[nodiscard]] static std::expected<Autosave, std::string> start(EventLog::Storage& storage, Editor& editor, const EventLog::Config& config = {});

    Autosave(Autosave&& other) noexcept
        : m_storage(other.m_storage), m_editor(std::exchange(other.m_editor, nullptr)), m_writer(std::move(other.m_writer)), m_config(other.m_config), m_ops(std::move(other.m_ops)) {}
    Autosave& operator=(Autosave&& other) noexcept {
        if (this != &other) {
            detach();
            m_storage = other.m_storage, m_editor = std::exchange(other.m_editor, nullptr), m_writer = std::move(other.m_writer), m_config = other.m_config, m_ops = std::move(other.m_ops);
        }
        return *this;
    }
    ~Autosave();

    /// @brief Делает устойчивую точку (flush журнала). Звать по таймеру/после значимых правок, не после каждой.
    bool flush();
    /// @brief Переписывает журнал с одним свежим снимком (журнал не растёт бесконечно).
    bool compact();
    [[nodiscard]] std::uint64_t operations_logged() const noexcept { return *m_ops; }

    struct Recovered {
        Graph graph;
        std::size_t operations = 0;            ///< Правок применено после снимка.
        std::size_t snapshots = 0;             ///< Снимков в журнале.
        EventLog::RecoveryReport report;
    };
    /// @brief Читает журнал (чиня по чётности) и собирает граф. Ошибка — нет ни одного снимка или журнал неузнаваем.
    [[nodiscard]] static std::expected<Recovered, std::string> recover(EventLog::Storage& storage);

private:
    Autosave(EventLog::Storage& storage, Editor& editor, EventLog::Writer writer, EventLog::Config config);
    void attach();
    void detach() {
        if (m_editor) m_editor->set_observer(nullptr);
    }
    void write_snapshot();

    EventLog::Storage* m_storage;
    Editor* m_editor;
    std::unique_ptr<EventLog::Writer> m_writer;
    EventLog::Config m_config;
    std::unique_ptr<std::uint64_t> m_ops = std::make_unique<std::uint64_t>(0);
};

} // namespace RuneEditor
