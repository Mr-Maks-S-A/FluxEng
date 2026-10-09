#include <AssetSystem/AssetId.hpp>
#include <AssetSystem/Crc32.hpp>
#include <AssetSystem/Vfs.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <unordered_set>

namespace AssetSystem {

static_assert(std::endian::native == std::endian::little, "AssetSystem formats are little-endian; add byte swaps for big-endian targets");

namespace fs = std::filesystem;

// ===================================================================== MemorySource

bool MemorySource::add(std::string_view path, Bytes data) {
    auto normalized = normalize_path(path);
    if (!normalized) return false;
    m_files[*normalized] = std::move(data);
    return true;
}

bool MemorySource::add_text(std::string_view path, std::string_view text) {
    Bytes bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return add(path, std::move(bytes));
}

Result<Bytes> MemorySource::read(std::string_view path) const {
    const auto it = m_files.find(std::string(path));
    if (it == m_files.end()) return fail(ErrorCode::NotFound, std::string(path));
    return it->second;
}

void MemorySource::list(std::vector<std::string>& out) const {
    for (const auto& [path, _] : m_files) out.push_back(path);
}

// ===================================================================== DirectorySource

DirectorySource::DirectorySource(fs::path root, std::size_t max_file_bytes)
    : m_max_file_bytes(max_file_bytes) {
    std::error_code ec;
    m_root = fs::weakly_canonical(root, ec);
    if (ec) m_root = std::move(root);
    m_name = m_root.generic_string();
}

Result<fs::path> DirectorySource::resolve(std::string_view path) const {
    std::error_code ec;
    const fs::path candidate = m_root / fs::path(std::string(path));
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec) return fail(ErrorCode::Io, ec.message());
    // Симлинк или хитрый путь не должны вывести за корень источника.
    const auto [root_end, canon_it] = std::mismatch(m_root.begin(), m_root.end(), canonical.begin(), canonical.end());
    if (root_end != m_root.end()) return fail(ErrorCode::InvalidPath, "path leaves the source root: " + std::string(path));
    return canonical;
}

bool DirectorySource::exists(std::string_view path) const {
    const auto resolved = resolve(path);
    std::error_code ec;
    return resolved && fs::is_regular_file(*resolved, ec);
}

Result<Bytes> DirectorySource::read(std::string_view path) const {
    const auto resolved = resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    if (!fs::is_regular_file(*resolved, ec)) return fail(ErrorCode::NotFound, std::string(path));
    const std::uintmax_t size = fs::file_size(*resolved, ec);
    if (ec) return fail(ErrorCode::Io, ec.message());
    if (size > m_max_file_bytes) return fail(ErrorCode::TooLarge, std::string(path) + " is larger than the file limit");
    std::ifstream file(*resolved, std::ios::binary);
    if (!file) return fail(ErrorCode::Io, "cannot open " + std::string(path));
    Bytes bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(file.gcount()) != bytes.size()) return fail(ErrorCode::Io, "short read of " + std::string(path));
    return bytes;
}

void DirectorySource::list(std::vector<std::string>& out) const {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string relative = fs::relative(it->path(), m_root, ec).generic_string();
        if (!ec && normalize_path(relative)) out.push_back(relative);
    }
}

// ===================================================================== Pack

namespace {

constexpr char kPackMagic[4] = {'F', 'X', 'P', 'K'};
constexpr std::uint32_t kPackVersion = 1;

template<typename T>
void put(Bytes& out, T value) {
    const std::size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : m_data(data) {}
    template<typename T>
    bool get(T& value) {
        if (m_data.size() - m_pos < sizeof(T)) return false;
        std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return true;
    }
    bool take(std::size_t count, std::string& out) {
        if (m_data.size() - m_pos < count) return false;
        out.assign(reinterpret_cast<const char*>(m_data.data() + m_pos), count);
        m_pos += count;
        return true;
    }
    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }

private:
    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
};

} // namespace

Result<std::unique_ptr<PackSource>> PackSource::open(Bytes data, std::string name, const AssetLimits& limits) {
    Reader in(data);
    char magic[4];
    std::uint32_t version = 0, count = 0, reserved = 0;
    for (char& c : magic) {
        std::uint8_t b = 0;
        if (!in.get(b)) return fail(ErrorCode::Corrupt, "pack is shorter than its header");
        c = static_cast<char>(b);
    }
    if (std::memcmp(magic, kPackMagic, 4) != 0) return fail(ErrorCode::Corrupt, "not a FXPK pack (bad magic)");
    if (!in.get(version) || !in.get(count) || !in.get(reserved)) return fail(ErrorCode::Corrupt, "pack is shorter than its header");
    if (version != kPackVersion) return fail(ErrorCode::Unsupported, "pack version " + std::to_string(version));
    if (count > limits.max_pack_entries) return fail(ErrorCode::TooLarge, "pack has too many entries");

    std::unique_ptr<PackSource> pack(new PackSource());
    pack->m_entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t path_len = 0;
        std::string path;
        Entry entry;
        if (!in.get(path_len) || path_len == 0 || !in.take(path_len, path) || !in.get(entry.offset) || !in.get(entry.size) || !in.get(entry.crc)) {
            return fail(ErrorCode::Corrupt, "pack entry table is truncated");
        }
        auto normalized = normalize_path(path);
        if (!normalized || *normalized != path) return fail(ErrorCode::Corrupt, "pack entry has a bad path");
        if (entry.offset > data.size() || entry.size > data.size() - entry.offset) {
            return fail(ErrorCode::Corrupt, "pack entry points outside the file: " + path);
        }
        if (!pack->m_entries.emplace(std::move(path), entry).second) return fail(ErrorCode::Corrupt, "pack has duplicate entries");
    }
    pack->m_name = std::move(name);
    pack->m_data = std::move(data);
    return pack;
}

Result<std::unique_ptr<PackSource>> PackSource::open_file(const fs::path& path, const AssetLimits& limits) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) return fail(ErrorCode::NotFound, path.generic_string());
    if (size > limits.max_file_bytes) return fail(ErrorCode::TooLarge, path.generic_string());
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(ErrorCode::Io, "cannot open " + path.generic_string());
    Bytes bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(file.gcount()) != bytes.size()) return fail(ErrorCode::Io, "short read of " + path.generic_string());
    return open(std::move(bytes), path.filename().generic_string(), limits);
}

Result<Bytes> PackSource::read(std::string_view path) const {
    const auto it = m_entries.find(std::string(path));
    if (it == m_entries.end()) return fail(ErrorCode::NotFound, std::string(path));
    const Entry& entry = it->second;
    const std::span<const std::byte> view(m_data.data() + entry.offset, static_cast<std::size_t>(entry.size));
    if (crc32(view) != entry.crc) return fail(ErrorCode::Corrupt, "checksum mismatch in pack entry " + std::string(path));
    return Bytes(view.begin(), view.end());
}

void PackSource::list(std::vector<std::string>& out) const {
    for (const auto& [path, _] : m_entries) out.push_back(path);
}

bool PackWriter::add(std::string_view path, Bytes data) {
    auto normalized = normalize_path(path);
    if (!normalized || normalized->size() > std::numeric_limits<std::uint16_t>::max()) return false;
    for (const auto& entry : m_entries) {
        if (entry.first == *normalized) return false;
    }
    m_entries.emplace_back(std::move(*normalized), std::move(data));
    return true;
}

Result<Bytes> PackWriter::build() const {
    std::uint64_t table = 16;
    for (const auto& [path, _] : m_entries) table += 2 + path.size() + 8 + 8 + 4;
    Bytes out;
    out.reserve(static_cast<std::size_t>(table));
    for (const char c : kPackMagic) put(out, static_cast<std::uint8_t>(c));
    put<std::uint32_t>(out, kPackVersion);
    put<std::uint32_t>(out, static_cast<std::uint32_t>(m_entries.size()));
    put<std::uint32_t>(out, 0);

    std::uint64_t offset = table;
    for (const auto& [path, data] : m_entries) {
        put<std::uint16_t>(out, static_cast<std::uint16_t>(path.size()));
        for (const char c : path) put(out, static_cast<std::uint8_t>(c));
        put<std::uint64_t>(out, offset);
        put<std::uint64_t>(out, data.size());
        put<std::uint32_t>(out, crc32(data));
        offset += data.size();
    }
    for (const auto& entry : m_entries) out.insert(out.end(), entry.second.begin(), entry.second.end());
    return out;
}

Result<void> PackWriter::write_file(const fs::path& path) const {
    auto bytes = build();
    if (!bytes) return std::unexpected(std::move(bytes.error()));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return fail(ErrorCode::Io, "cannot create " + path.generic_string());
    file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!file) return fail(ErrorCode::Io, "write failed: " + path.generic_string());
    return {};
}

// ===================================================================== VirtualFileSystem

IFileSource& VirtualFileSystem::mount(std::unique_ptr<IFileSource> source) {
    m_sources.push_back(std::move(source));
    return *m_sources.back();
}

Result<Bytes> VirtualFileSystem::read(std::string_view path) const {
    const auto normalized = normalize_path(path);
    if (!normalized) return std::unexpected(normalized.error());
    for (auto it = m_sources.rbegin(); it != m_sources.rend(); ++it) {
        if ((*it)->exists(*normalized)) return (*it)->read(*normalized);
    }
    return fail(ErrorCode::NotFound, *normalized);
}

bool VirtualFileSystem::exists(std::string_view path) const {
    const auto normalized = normalize_path(path);
    if (!normalized) return false;
    return std::ranges::any_of(m_sources, [&](const auto& source) { return source->exists(*normalized); });
}

std::vector<std::string> VirtualFileSystem::list() const {
    std::set<std::string> unique;
    std::vector<std::string> chunk;
    for (const auto& source : m_sources) {
        chunk.clear();
        source->list(chunk);
        unique.insert(chunk.begin(), chunk.end());
    }
    return {unique.begin(), unique.end()};
}

} // namespace AssetSystem
