#pragma once
/**
 * @file Lockstep.hpp
 * @brief Пошаговая синхронизация (lockstep): все пиры исполняют одни и те же команды на одних и тех же тиках.
 *
 * Симуляция детерминирована (Fixed, сид, порядок фаз), значит по сети нужно гонять не состояние мира, а **ввод**.
 * Каждый пир шлёт свои команды на тик `T + input_delay`; тик `T` исполняется, когда команды **всех** пиров на него есть.
 * Команды склеиваются в порядке номеров пиров — одинаково у всех. Задержка ввода прячет сетевую задержку:
 * пока команда летит, пир исполняет предыдущие тики.
 *
 * ```
 *  каждый кадр:
 *    net.pump();                                  // принять пакеты, отправить/повторить свои
 *    if (net.needs_input()) net.submit(local);    // свои команды на тик tick()+input_delay
 *    if (net.ready()) {                           // команды всех пиров на tick() есть
 *        sim.tick(net.advance());                 // одинаково у всех
 *        net.report_hashes(sim.hashes());         // сверка именованных хешей: расхождение видно сразу и по подсистеме
 *    }                                            // не готово — кадр без шага (сеть отстаёт), игра не падает
 * ```
 *
 * **Надёжность поверх ненадёжной сети.** Каждый пакет `Inputs` несёт *все* ещё не подтверждённые тики и подтверждение
 * принятых; потерянный пакет «лечится» следующим, переупорядочение и дубликаты безвредны. Большие данные (программа
 * заклинания для `SetProgram`) идут блобами по хешу с подтверждением. Команда, ссылающаяся на блоб (`set_blob_reference`),
 * не исполняется, пока блоб не пришёл и не сошёлся с хешем.
 *
 * **Топологии.** По умолчанию каждый связан с каждым напрямую (**mesh**, p2p): каждый шлёт только свои команды.
 * Для **звезды** клиентам указывают одного соседа — сервер (`neighbors = {0}`), а серверу `relay = true`: он пересылает
 * команды, блобы и подтверждения между клиентами и сам тоже симулирует. Хеши состояния сверяются между соседями:
 * в звезде клиенты видят только сервер, а у них с ним всё сходится, пока сходится со всеми.
 *
 * **Доверие.** Все пакеты проверяются (`Net::decode`), но пиры считаются честными: чужой «мёртвый» блоб или молчание
 * останавливают игру (`stalled_pumps()`), а не портят её; исключение пира и защита от нечестной игры — дело следующего слоя.
 */

#include <Net/Transport.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace Net {

struct LockstepConfig {
    PeerId local = 0;
    std::uint8_t peers = 2;
    std::uint64_t seed = 0;                  ///< Сид мира: у всех одинаковый (проверяется при знакомстве).
    std::uint64_t config_hash = 0;           ///< Хеш настройки (уровень, версия правил): у всех одинаковый.
    std::uint32_t input_delay = 3;           ///< Тиков между вводом и исполнением (3 тика = 50 мс при 60 Гц).
    std::uint32_t resend_interval = 4;       ///< Как часто (в `pump`) напоминать о себе, если нечего слать.
    std::size_t max_blob_chunks_per_pump = 2;///< Лимит кусков блоба на пира за `pump` (чтобы блоб не заглушил ввод).
    std::size_t max_stored_blobs = 256;
    std::uint32_t max_window = 24;           ///< Тиков одного игрока в одном пакете.
    std::vector<PeerId> neighbors{};         ///< С кем связаны напрямую; пусто — со всеми остальными (mesh).
    bool relay = false;                      ///< Пересылать соседям чужие команды и блобы (сервер звезды).
};

/// @brief Расхождение состояний: первый тик и пир, у которого хеши отличаются, и названия разошедшихся подсистем.
struct Desync {
    std::uint32_t tick = 0;
    PeerId peer = 0;
    std::vector<std::string> subsystems;
};

struct LockstepStats {
    std::uint64_t packets_sent = 0, packets_received = 0, bad_packets = 0, bytes_sent = 0;
    std::uint64_t stalled_pumps = 0;      ///< Сколько раз `pump` застал игру неготовой к шагу.
    std::uint64_t blobs_received = 0, blobs_rejected = 0;
    std::uint64_t input_conflicts = 0;    ///< Две разные команды одного игрока на один тик (в честной сети — всегда 0).
};

class Lockstep {
public:
    /// Какой блоб (хеш) нужен команде, чтобы её можно было исполнять. `nullopt` — никакой.
    using BlobReference = std::function<std::optional<std::uint64_t>(const Replay::Command&)>;

    Lockstep(Transport& transport, const LockstepConfig& config);

    void set_blob_reference(BlobReference reference) { m_blob_reference = std::move(reference); }
    [[nodiscard]] const BlobReference& blob_reference() const noexcept { return m_blob_reference; }

    /// @brief Принять пакеты, ответить, отправить свои новые и неподтверждённые данные, подтверждения блобов.
    void pump();

    /// @brief Все пиры познакомились и сошлись по настройке (иначе `ready()` ложно). Расхождение — `failure()`.
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const std::string& failure() const noexcept { return m_failure; }

    /// @brief Пора подать свои команды (на тик `tick() + input_delay`). Один вызов `submit` — один тик.
    [[nodiscard]] bool needs_input() const noexcept { return m_local_next <= m_tick + m_config.input_delay; }
    /// @brief Свои команды на следующий неподанный тик (можно пустой span). Возвращает, сколько принято (≤ 16 на тик).
    std::size_t submit(std::span<const Replay::Command> commands);
    /// @brief Рассылает блоб и сохраняет у себя; возвращает хеш для команды. Подавать до команды со ссылкой.
    std::uint64_t submit_blob(std::span<const std::byte> bytes);

    /// @brief Команды всех пиров на `tick()` есть (и их блобы тоже).
    [[nodiscard]] bool ready() const;
    /// @brief Склеенные команды тика `tick()` (порядок пиров, внутри — порядок подачи) и переход к следующему тику.
    /// Span действителен до следующего `advance`.
    [[nodiscard]] std::span<const Replay::Command> advance();
    /// @brief На какой тик попадут команды следующего `submit` (тик ввода = исполняемый + задержка).
    [[nodiscard]] std::uint32_t next_input_tick() const noexcept { return m_local_next; }
    /// @brief Следующий тик к исполнению (= число уже исполненных).
    [[nodiscard]] std::uint32_t tick() const noexcept { return m_tick; }

    /// @brief Хеши после последнего исполненного тика: рассылаются остальным и сверяются с их хешами того же тика.
    void report_hashes(const Replay::StateHashes& hashes);
    [[nodiscard]] const std::optional<Desync>& desync() const noexcept { return m_desync; }

    [[nodiscard]] const std::vector<std::byte>* blob(std::uint64_t hash) const;
    [[nodiscard]] const LockstepStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const LockstepConfig& config() const noexcept { return m_config; }
    /// @brief Соседи (с кем идёт обмен напрямую).
    [[nodiscard]] const std::vector<PeerId>& neighbors() const noexcept { return m_neighbors; }

    /**
     * @brief Сколько вызовов `pump` прошло с последнего принятого от соседа действительного пакета (0 — слышали только что).
     * Для пира, который ещё ни разу не отзывался, отсчёт идёт с создания узла. Не сосед и свой номер — 0.
     * Это **детектор**: сеть молчит — решает слой выше (ждать, показать «ожидание игрока N», завершить партию).
     * Исключить пира из партии в lockstep можно только по общему решению всех: у каждого свой «последний слышал».
     */
    [[nodiscard]] std::uint32_t pumps_since_heard(PeerId peer) const noexcept;
    /// @brief Соседи, молчащие дольше `threshold_pumps` (по возрастанию номера).
    [[nodiscard]] std::vector<PeerId> silent_peers(std::uint32_t threshold_pumps) const;

private:
    struct OutgoingBlob {
        std::uint64_t hash = 0;
        std::vector<std::byte> bytes;
        std::vector<bool> acked;               ///< По пирам (не соседи, свой номер и источник — сразу true).
        std::vector<std::uint32_t> next_offset;///< По пирам: с какого места слать дальше.
    };
    struct Partial {
        std::uint32_t total = 0;
        std::vector<std::byte> data;
        std::set<std::uint32_t> offsets;
        std::uint32_t received = 0;
    };

    void send_packet(PeerId to, const Body& body);
    void handle(const Packet& packet);
    void send_hello();
    void send_inputs(PeerId to, bool periodic);
    void make_outgoing(std::uint64_t hash, std::span<const std::byte> bytes, PeerId except);
    void send_blobs();
    void compare(std::uint32_t tick, PeerId peer, const Replay::StateHashes& theirs);
    [[nodiscard]] std::size_t index(PeerId p) const noexcept { return p; }

    Transport& m_transport;
    LockstepConfig m_config;
    BlobReference m_blob_reference;

    std::uint32_t m_tick = 0;
    std::uint32_t m_local_next = 0;                       ///< Первый ещё не поданный тик.
    std::vector<std::map<std::uint32_t, std::vector<Replay::Command>>> m_inputs; ///< По пирам: команды по тикам.
    std::vector<std::uint32_t> m_have;                    ///< По пирам: тики [0, have) есть подряд.
    std::vector<std::vector<std::uint32_t>> m_peer_have;  ///< [пир][игрок]: сколько тиков игрока у пира есть подряд (его подтверждения).
    std::vector<PeerId> m_neighbors;
    std::vector<bool> m_is_neighbor;
    std::vector<bool> m_hello_seen;
    std::vector<std::vector<std::uint32_t>> m_sent_have;  ///< [сосед]: какие подтверждения (m_have) ушли ему в последний раз.
    std::string m_failure;
    std::uint32_t m_pumps = 0;
    std::vector<std::uint32_t> m_last_heard;              ///< По пирам: номер `pump`, на котором пришёл их последний действительный пакет.

    std::map<std::uint64_t, std::vector<std::byte>> m_blobs;
    std::map<std::uint64_t, Partial> m_partial;
    std::vector<OutgoingBlob> m_outgoing;

    std::map<std::uint32_t, Replay::StateHashes> m_my_hashes;
    std::map<std::pair<std::uint32_t, PeerId>, Replay::StateHashes> m_their_hashes;
    std::optional<Desync> m_desync;

    std::vector<Replay::Command> m_merged;
    LockstepStats m_stats;
};

} // namespace Net
