#pragma once
/**
 * @file Storage.hpp
 * @brief Куда пишется журнал: интерфейс хранилища байт и две реализации — память (тесты, имитация повреждений) и файл.
 *
 * Журнал не знает, что под ним: файл, раздел диска, сетевой том. Интерфейс — минимум, нужный для журнала с произвольным
 * доступом: размер, чтение и запись по смещению, усечение, сброс на устойчивый носитель.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace EventLog {

class Storage {
public:
    virtual ~Storage() = default;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    /// @brief Читает до `out.size()` байт с `offset`; возвращает, сколько прочитано (меньше — конец хранилища).
    [[nodiscard]] virtual std::size_t read(std::uint64_t offset, std::span<std::byte> out) const = 0;
    /// @brief Пишет байты с `offset`; запись за концом увеличивает размер (дыра заполняется нулями).
    virtual bool write(std::uint64_t offset, std::span<const std::byte> data) = 0;
    virtual bool truncate(std::uint64_t new_size) = 0;
    /// @brief Сбрасывает записанное на устойчивый носитель (для файла — fflush + fsync).
    virtual bool flush() = 0;
};

/// @brief Хранилище в памяти. `bytes()` открыт намеренно: тесты портят отдельные байты, имитируя сбои носителя.
class MemoryStorage final : public Storage {
public:
    [[nodiscard]] std::uint64_t size() const override { return m_bytes.size(); }
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) const override;
    bool write(std::uint64_t offset, std::span<const std::byte> data) override;
    bool truncate(std::uint64_t new_size) override;
    bool flush() override { return true; }
    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return m_bytes; }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
};

/// @brief Файл на диске (двоичный, чтение и запись).
class FileStorage final : public Storage {
public:
    enum class Mode : std::uint8_t {
        Create,  ///< Создать новый (существующий обнуляется).
        Existing ///< Открыть существующий для чтения и дозаписи.
    };
    /// @return `nullptr`, если файл не открылся.
    [[nodiscard]] static std::unique_ptr<FileStorage> open(const std::filesystem::path& path, Mode mode);
    ~FileStorage() override;
    FileStorage(const FileStorage&) = delete;
    FileStorage& operator=(const FileStorage&) = delete;

    [[nodiscard]] std::uint64_t size() const override;
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) const override;
    bool write(std::uint64_t offset, std::span<const std::byte> data) override;
    bool truncate(std::uint64_t new_size) override;
    bool flush() override;

private:
    FileStorage(std::FILE* file, std::filesystem::path path) : m_file(file), m_path(std::move(path)) {}
    std::FILE* m_file;
    std::filesystem::path m_path;
};

} // namespace EventLog
