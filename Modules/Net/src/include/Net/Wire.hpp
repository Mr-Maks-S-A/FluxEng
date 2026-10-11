#pragma once
/**
 * @file Wire.hpp
 * @brief Пакеты сети: двоичный формат с контрольной суммой, разбор без доверия к входным байтам.
 *
 * ```
 *   [ 'F''X' | версия u8 | вид u8 | от кого u8 | тело ... | CRC-32C u32 ]      всё little-endian
 * ```
 * Пять видов пакетов:
 * - `Hello` — «я пир N из M, сид, хеш настройки»: проверка, что все играют в одно и то же;
 * - `Inputs` — команды тиков одного или нескольких игроков (все ещё не подтверждённые получателем) + подтверждение
 *   «у меня есть тики игрока j до …» по каждому игроку: потеря пакета лечится следующим, отдельных повторных запросов нет;
 *   чужие игроки в пакете бывают, когда узел ретранслирует (сервер звезды);
 * - `BlobChunk` / `BlobAck` — большие данные (программа заклинания) кусками по хешу содержимого;
 * - `HashReport` — именованные хеши подсистем на тике: так видно расхождение и какая подсистема разошлась.
 *
 * `decode` — граница доверия: неверная длина, контрольная сумма, версия, числа за пределами — отказ с текстом,
 * без исключений и без выхода за буфер. Пакет не больше `max_packet_size` (с запасом под MTU).
 */

#include <Replay/Replay.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Net {

using PeerId = std::uint8_t;
constexpr std::uint8_t protocol_version = 2;
constexpr std::size_t max_peers = 16;               ///< Игроков в одной партии.
constexpr std::size_t max_packet_size = 1200;       ///< Влезает в один UDP-датаграммный MTU без фрагментации.
constexpr std::size_t max_commands_per_tick = 16;   ///< Команд одного пира на тик (остальное — в следующий).
constexpr std::size_t max_blob_size = 64 * 1024;
constexpr std::size_t blob_chunk_size = 1000;

struct Hello {
    std::uint8_t peer_count = 0;
    std::uint64_t seed = 0;
    std::uint64_t config_hash = 0; ///< Хеш всего, что должно совпасть у всех (уровень, версия правил).
    bool knows_you = false;        ///< «Ваше Hello я уже получил»: по `false` получатель отвечает, чтобы знакомство не зависло от потери пакета.
};

struct TickInputs {
    std::uint32_t tick = 0;
    std::vector<Replay::Command> commands;
};

/// @brief Тики одного игрока: подряд идущие, начиная с первого неподтверждённого получателем.
struct PlayerTicks {
    PeerId player = 0;
    std::vector<TickInputs> ticks;
};

struct Inputs {
    std::vector<std::uint32_t> have; ///< По игрокам: «у меня подряд есть их тики [0, have[j])». Длина = число игроков.
    std::vector<PlayerTicks> players;///< Свои тики и (при ретрансляции) чужие.
};

struct BlobChunk {
    std::uint64_t hash = 0;
    std::uint32_t total = 0;
    std::uint32_t offset = 0;
    std::vector<std::byte> data;
};

struct BlobAck { std::uint64_t hash = 0; };

struct HashReport {
    std::uint32_t tick = 0;
    Replay::StateHashes hashes;
};

using Body = std::variant<Hello, Inputs, BlobChunk, BlobAck, HashReport>;

struct Packet {
    PeerId from = 0;
    Body body;
};

[[nodiscard]] std::vector<std::byte> encode(const Packet& packet);
[[nodiscard]] std::expected<Packet, std::string> decode(std::span<const std::byte> bytes);

} // namespace Net
