#pragma once
/**
 * @file EventBus.hpp
 * @brief Шина событий: каналы, тики, модули и граф зависимостей.
 */

#include <EventSystem/Bus/EventReader.hpp>
#include <EventSystem/Bus/EventWriter.hpp>
#include <EventSystem/Channel/Channel.hpp>
#include <EventSystem/Channel/IChannel.hpp>
#include <EventSystem/Channel/StreamChannel.hpp>
#include <EventSystem/Core/Event.hpp>
#include <EventSystem/Core/Ids.hpp>
#include <EventSystem/Core/Schema.hpp>
#include <EventSystem/Graph/EventGraph.hpp>
#include <EventSystem/Graph/ModuleRegistry.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace EventSystem {

class ModuleBuilder;

/// @brief Запись журнала причин: событие, его причина и тип (для дерева «что из чего выросло»).
struct TraceRecord {
    EventRef ref{};    ///< Событие.
    EventRef cause{};  ///< Его причина (нулевая — корень: ввод, таймер, система без причины).
    EventId event{};   ///< Тип события.
};

/**
 * @brief Центральная шина событий.
 *
 * Шина владеет каналами (по одному на тип события), считает тики и хранит
 * декларации модулей. Сама она не знает ни одного конкретного события:
 * события объявляются модулями в их публичных заголовках-контрактах.
 *
 * Типичный цикл:
 * @code
 * EventBus bus;
 * ModuleId physics = bus.declare_module("Physics").produces<CollisionEvent>();
 * ModuleId combat  = bus.declare_module("Combat").consumes<CollisionEvent>();
 *
 * auto collisions_out = bus.writer<CollisionEvent>(physics); // проверяет декларацию
 * auto collisions_in  = bus.reader<CollisionEvent>(combat);
 *
 * while (running) {
 *     physics_system(collisions_out);  // emit(...)
 *     combat_system(collisions_in);    // читает события прошлого тика
 *     bus.advance_tick();
 * }
 * @endcode
 *
 * Обработка ошибок: регистрация и получение писателей/читателей бросают
 * EventSystemError; горячий путь (emit, чтение, advance_tick) исключений не бросает.
 *
 * @note Не потокобезопасна. В бета-версии вся работа с шиной идёт из одного потока.
 */
class EventBus {
public:
    EventBus() = default;
    ~EventBus() = default;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) noexcept = default;
    EventBus& operator=(EventBus&&) noexcept = default;

    // ================================================================= регистрация

    /**
     * @brief Регистрирует C++-событие `E`.
     *
     * Повторная регистрация того же события безопасна и возвращает существующий канал;
     * если передан `config`, он применяется к существующему каналу.
     *
     * @param config Настройки канала; `std::nullopt` — значения по умолчанию
     *               (для нового канала) или «не менять» (для существующего).
     * @throws EventSystemError Схема некорректна (см. validate_schema()), имя занято событием
     *         с другой схемой, коллизия хешей имён или попытка сменить политику доставки.
     */
    template<Event E>
    IChannel& register_event(std::optional<ChannelConfig> config = std::nullopt) {
        return register_schema(schema_of<E>(), config);
    }

    /**
     * @brief Регистрирует событие по рантайм-схеме (скрипты, моды, данные).
     * @copydetails register_event
     */
    IChannel& register_schema(const EventSchema& schema, std::optional<ChannelConfig> config = std::nullopt);

    // ================================================================= поиск

    /// @brief Канал по ID или `nullptr`.
    [[nodiscard]] IChannel* find(EventId id) noexcept;
    /// @copydoc find(EventId)
    [[nodiscard]] const IChannel* find(EventId id) const noexcept;
    /// @brief Канал по имени события или `nullptr`.
    [[nodiscard]] IChannel* find(std::string_view name) noexcept;
    /// @copydoc find(std::string_view)
    [[nodiscard]] const IChannel* find(std::string_view name) const noexcept;

    /// @brief `true`, если событие `E` зарегистрировано.
    template<Event E>
    [[nodiscard]] bool contains() const noexcept { return find(event_id_v<E>) != nullptr; }

    /// @brief Количество каналов.
    [[nodiscard]] std::size_t channel_count() const noexcept { return m_channels.size(); }

    /// @brief Канал по индексу в порядке регистрации (для инструментов и отладочного UI).
    [[nodiscard]] IChannel& channel_at(std::size_t index) noexcept { return *m_channels[index]; }
    /// @copydoc channel_at
    [[nodiscard]] const IChannel& channel_at(std::size_t index) const noexcept { return *m_channels[index]; }

    // ================================================================= типизированный доступ

    /**
     * @brief Писатель событий `E`. Получите один раз и храните в системе.
     * @throws EventSystemError Событие не зарегистрировано или его схема не совпадает с `E`.
     */
    template<Event E>
    [[nodiscard]] EventWriter<E> writer() {
        return EventWriter<E>(channel_of(schema_of<E>()));
    }

    /**
     * @brief Читатель событий `E`. Получите один раз и храните в системе.
     * @throws EventSystemError Событие не зарегистрировано или его схема не совпадает с `E`.
     */
    template<Event E>
    [[nodiscard]] EventReader<E> reader() const {
        return EventReader<E>(channel_of(schema_of<E>()));
    }

    /**
     * @brief Писатель с проверкой контракта: модуль должен был объявить `produces<E>()`.
     * @throws EventSystemError Модуль не объявлен или не объявил событие.
     */
    template<Event E>
    [[nodiscard]] EventWriter<E> writer(ModuleId producer) {
        require_declared(producer, event_id_v<E>, Role::Producer);
        return writer<E>();
    }

    /**
     * @brief Читатель с проверкой контракта: модуль должен был объявить `consumes<E>()`.
     * @throws EventSystemError Модуль не объявлен или не объявил событие.
     */
    template<Event E>
    [[nodiscard]] EventReader<E> reader(ModuleId consumer) const {
        require_declared(consumer, event_id_v<E>, Role::Consumer);
        return reader<E>();
    }

    // ================================================================= время

    /**
     * @brief Завершает тик: в каналах домена Domain::Tick отправленное становится видимым
     *        (по правилам политики), дорожки потоков сливаются, события прошлого тика удаляются.
     */
    void advance_tick();

    /// @brief Завершает кадр: то же для каналов домена Domain::Frame (идёт и на паузе симуляции).
    void advance_frame();

    /// @brief Номер текущего тика (начинается с 0).
    [[nodiscard]] Tick current_tick() const noexcept { return m_tick; }
    /// @brief Номер текущего кадра (сколько раз вызван advance_frame()).
    [[nodiscard]] Tick current_frame() const noexcept { return m_frame; }

    // ================================================================= дерево причин (ChannelConfig::trace)

    /// @brief Сколько последних записей хранит журнал причин (по умолчанию 65 536; 0 — не хранить).
    void set_trace_capacity(std::size_t records);
    /// @brief Журнал: записи о событиях трассируемых каналов в порядке появления (старые вытесняются).
    [[nodiscard]] std::vector<TraceRecord> trace_journal() const;
    /// @brief Цепочка причин от корня до события `ref` включительно (пусто, если события нет в журнале).
    [[nodiscard]] std::vector<TraceRecord> cause_chain(EventRef ref) const;
    /// @brief Прямые следствия события `ref`.
    [[nodiscard]] std::vector<TraceRecord> effects(EventRef ref) const;
    /// @brief Дерево «событие → следствия → …» текстом, до глубины `depth`.
    [[nodiscard]] std::string trace_tree(EventRef root, int depth = 8) const;
    /// @brief «имя@время#номер» для вывода.
    [[nodiscard]] std::string describe(EventRef ref) const;

    /// @brief Удаляет все события во всех каналах (например при загрузке сохранения).
    void clear_all() noexcept;

    // ================================================================= модули

    /**
     * @brief Объявляет модуль и возвращает построитель его контракта.
     * @throws EventSystemError Имя пустое или уже занято.
     */
    [[nodiscard]] ModuleBuilder declare_module(std::string_view name);

    /// @brief Реестр модулей.
    [[nodiscard]] const ModuleRegistry& modules() const noexcept { return m_modules; }

    /// @brief Снимок графа зависимостей по текущим декларациям и каналам.
    [[nodiscard]] EventGraph build_graph() const;

private:
    friend class ModuleBuilder;

    enum class Role : unsigned char { Producer, Consumer };

    [[nodiscard]] Channel& channel_of(const EventSchema& schema);
    [[nodiscard]] const Channel& channel_of(const EventSchema& schema) const;
    void advance(Domain domain);
    void record_trace(const Channel& channel);
    void require_declared(ModuleId module, EventId event, Role role) const;
    void declare_link(ModuleId module, EventId event, Role role);

    std::vector<std::unique_ptr<Channel>> m_channels;
    std::unordered_map<EventId, std::size_t> m_index;
    ModuleRegistry m_modules;
    Tick m_tick = 0;
    Tick m_frame = 0;
    std::vector<TraceRecord> m_journal; ///< Кольцо записей причин.
    std::size_t m_journal_head = 0;
    std::size_t m_journal_capacity = 1u << 16;
};

/**
 * @brief Построитель контракта модуля: какие события он порождает и потребляет.
 *
 * Объявление события в контракте заодно регистрирует его канал в шине,
 * поэтому отдельный вызов EventBus::register_event() обычно не нужен.
 *
 * @code
 * ModuleId magic = bus.declare_module("Magic")
 *     .produces<SpellCastEvent>(ChannelConfig{.reserve = 256, .max_events_per_tick = 4096})
 *     .consumes<CollisionEvent>();
 * @endcode
 */
class ModuleBuilder {
public:
    /// @brief Используйте EventBus::declare_module().
    ModuleBuilder(EventBus& bus, ModuleId module) noexcept : m_bus(&bus), m_module(module) {}

    /**
     * @brief Модуль порождает событие `E`.
     * @param config Настройки канала (см. EventBus::register_event()).
     */
    template<Event E>
    ModuleBuilder& produces(std::optional<ChannelConfig> config = std::nullopt) {
        m_bus->register_event<E>(config);
        m_bus->declare_link(m_module, event_id_v<E>, EventBus::Role::Producer);
        return *this;
    }

    /// @brief Модуль потребляет событие `E`.
    template<Event E>
    ModuleBuilder& consumes() {
        m_bus->register_event<E>();
        m_bus->declare_link(m_module, event_id_v<E>, EventBus::Role::Consumer);
        return *this;
    }

    /**
     * @brief Модуль порождает уже зарегистрированное событие (например описанное скриптом).
     * @throws EventSystemError Событие не зарегистрировано.
     */
    ModuleBuilder& produces(EventId event);

    /**
     * @brief Модуль потребляет уже зарегистрированное событие.
     * @throws EventSystemError Событие не зарегистрировано.
     */
    ModuleBuilder& consumes(EventId event);

    /// @brief Идентификатор модуля.
    [[nodiscard]] ModuleId id() const noexcept { return m_module; }

    /// @brief Неявное преобразование, чтобы писать `ModuleId m = bus.declare_module("X").produces<E>();`.
    operator ModuleId() const noexcept { return m_module; } // NOLINT(google-explicit-constructor)

private:
    EventBus* m_bus;
    ModuleId m_module;
};

} // namespace EventSystem
