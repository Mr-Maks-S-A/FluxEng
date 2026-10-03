#include <EventLog/Storage.hpp>

#include <algorithm>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace EventLog {

std::size_t MemoryStorage::read(std::uint64_t offset, std::span<std::byte> out) const {
    if (offset >= m_bytes.size()) return 0;
    const std::size_t n = std::min<std::size_t>(out.size(), m_bytes.size() - static_cast<std::size_t>(offset));
    std::copy_n(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
    return n;
}

bool MemoryStorage::write(std::uint64_t offset, std::span<const std::byte> data) {
    const std::size_t end = static_cast<std::size_t>(offset) + data.size();
    if (end > m_bytes.size()) m_bytes.resize(end, std::byte{0});
    std::copy(data.begin(), data.end(), m_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

bool MemoryStorage::truncate(std::uint64_t new_size) {
    m_bytes.resize(static_cast<std::size_t>(new_size), std::byte{0});
    return true;
}

std::unique_ptr<FileStorage> FileStorage::open(const std::filesystem::path& path, Mode mode) {
    std::FILE* file = std::fopen(path.string().c_str(), mode == Mode::Create ? "w+b" : "r+b");
    if (!file) return nullptr;
    return std::unique_ptr<FileStorage>(new FileStorage(file, path));
}

FileStorage::~FileStorage() {
    if (m_file) std::fclose(m_file);
}

std::uint64_t FileStorage::size() const {
    std::fflush(m_file);
    std::error_code ec;
    const auto s = std::filesystem::file_size(m_path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(s);
}

std::size_t FileStorage::read(std::uint64_t offset, std::span<std::byte> out) const {
    std::fflush(m_file); // запись и чтение идут через один буфер stdio: переключение требует сброса
    if (std::fseek(m_file, static_cast<long>(offset), SEEK_SET) != 0) return 0;
    return std::fread(out.data(), 1, out.size(), m_file);
}

bool FileStorage::write(std::uint64_t offset, std::span<const std::byte> data) {
    const std::uint64_t current = size();
    if (offset > current) { // дыра: заполняем нулями, как MemoryStorage
        if (std::fseek(m_file, static_cast<long>(current), SEEK_SET) != 0) return false;
        const std::vector<std::byte> zeros(static_cast<std::size_t>(offset - current), std::byte{0});
        if (std::fwrite(zeros.data(), 1, zeros.size(), m_file) != zeros.size()) return false;
    }
    if (std::fseek(m_file, static_cast<long>(offset), SEEK_SET) != 0) return false;
    return std::fwrite(data.data(), 1, data.size(), m_file) == data.size();
}

bool FileStorage::truncate(std::uint64_t new_size) {
    std::fflush(m_file);
#if defined(__unix__) || defined(__APPLE__)
    return ::ftruncate(::fileno(m_file), static_cast<off_t>(new_size)) == 0;
#else
    std::error_code ec;
    std::filesystem::resize_file(m_path, new_size, ec);
    return !ec;
#endif
}

bool FileStorage::flush() {
    if (std::fflush(m_file) != 0) return false;
#if defined(__unix__) || defined(__APPLE__)
    return ::fsync(::fileno(m_file)) == 0;
#else
    return true;
#endif
}

} // namespace EventLog
