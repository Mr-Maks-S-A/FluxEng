#pragma once
/**
 * @file Vfs.hpp
 * @brief Виртуальная файловая система: источники файлов (память, каталог, пак) смонтированы друг на друга.
 *
 * ```
 * VirtualFileSystem vfs;
 * vfs.mount(PackSource::open_file("base.fxpk"));          // базовая игра
 * vfs.mount(std::make_unique<DirectorySource>("mods/x")); // позже смонтированное перекрывает раннее
 * auto bytes = vfs.read("models/staff.fmesh");
 * ```
 *
 * Потоки: монтировать — до начала загрузок; `read`/`exists`/`list` потокобезопасны (источники неизменяемы).
 * Пути проходят normalize_path, поэтому `..` не выводит за корень источника.
 */

#include <AssetSystem/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AssetSystem {

using Bytes = std::vector<std::byte>;

/// @brief Источник файлов. Пути приходят уже нормализованными.
class IFileSource {
public:
    virtual ~IFileSource() = default;
    [[nodiscard]] virtual Result<Bytes> read(std::string_view path) const = 0;
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
    /// @brief Добавляет в `out` пути всех файлов источника.
    virtual void list(std::vector<std::string>& out) const = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

/// @brief Файлы в памяти: тесты, ассеты, сгенерированные кодом, данные от сервера.
class MemorySource final : public IFileSource {
public:
    explicit MemorySource(std::string name = "memory") : m_name(std::move(name)) {}

    /// @return false, если путь не прошёл нормализацию.
    bool add(std::string_view path, Bytes data);
    bool add_text(std::string_view path, std::string_view text);

    [[nodiscard]] Result<Bytes> read(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override { return m_files.contains(std::string(path)); }
    void list(std::vector<std::string>& out) const override;
    [[nodiscard]] std::string_view name() const noexcept override { return m_name; }

private:
    std::string m_name;
    std::unordered_map<std::string, Bytes> m_files;
};

/// @brief Каталог на диске. Файл вне корня (в том числе через симлинк) недоступен.
class DirectorySource final : public IFileSource {
public:
    explicit DirectorySource(std::filesystem::path root, std::size_t max_file_bytes = std::size_t{256} << 20);

    [[nodiscard]] Result<Bytes> read(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    void list(std::vector<std::string>& out) const override;
    [[nodiscard]] std::string_view name() const noexcept override { return m_name; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

private:
    [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view path) const;

    std::filesystem::path m_root;
    std::string m_name;
    std::size_t m_max_file_bytes;
};

/// @brief Пак `.fxpk`: один файл с таблицей записей и контрольными суммами. Читается из памяти.
///
/// Формат (little-endian): `"FXPK" u32 version=1 u32 count u32 reserved`, затем `count` записей
/// `u16 path_len, path, u64 offset, u64 size, u32 crc32`, затем данные. Смещения — от начала файла.
class PackSource final : public IFileSource {
public:
    /// @brief Разбирает пак. Проверяет границы, дубликаты и пути записей; CRC данных проверяется при чтении.
    [[nodiscard]] static Result<std::unique_ptr<PackSource>> open(Bytes data, std::string name = "pack",
                                                                  const AssetLimits& limits = {});
    [[nodiscard]] static Result<std::unique_ptr<PackSource>> open_file(const std::filesystem::path& path,
                                                                       const AssetLimits& limits = {});

    [[nodiscard]] Result<Bytes> read(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override { return m_entries.contains(std::string(path)); }
    void list(std::vector<std::string>& out) const override;
    [[nodiscard]] std::string_view name() const noexcept override { return m_name; }
    [[nodiscard]] std::size_t entry_count() const noexcept { return m_entries.size(); }

private:
    struct Entry {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
    };
    PackSource() = default;

    std::string m_name;
    Bytes m_data;
    std::unordered_map<std::string, Entry> m_entries;
};

/// @brief Собирает пак из файлов в памяти. Порядок записей — порядок add(): пак воспроизводим побайтно.
class PackWriter {
public:
    /// @return false — путь не нормализуется или уже добавлен.
    bool add(std::string_view path, Bytes data);
    [[nodiscard]] Result<Bytes> build() const;
    [[nodiscard]] Result<void> write_file(const std::filesystem::path& path) const;
    [[nodiscard]] std::size_t entry_count() const noexcept { return m_entries.size(); }

private:
    std::vector<std::pair<std::string, Bytes>> m_entries;
};

/// @brief Стек источников; поздно смонтированный перекрывает ранний.
class VirtualFileSystem {
public:
    /// @brief Монтирует источник поверх остальных. @return ссылка на него.
    IFileSource& mount(std::unique_ptr<IFileSource> source);

    [[nodiscard]] Result<Bytes> read(std::string_view path) const;
    [[nodiscard]] bool exists(std::string_view path) const;
    /// @brief Все пути всех источников, без повторов, по возрастанию.
    [[nodiscard]] std::vector<std::string> list() const;
    [[nodiscard]] std::size_t mount_count() const noexcept { return m_sources.size(); }

private:
    std::vector<std::unique_ptr<IFileSource>> m_sources;
};

} // namespace AssetSystem
