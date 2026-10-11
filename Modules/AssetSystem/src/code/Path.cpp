#include <AssetSystem/AssetId.hpp>

#include <vector>

namespace AssetSystem {

namespace {

// Быстрый путь: путь уже в нормальном виде (самый частый случай — его выдают сами ассеты и паки), аллокаций нет, кроме результата.
bool already_normalized(std::string_view raw) noexcept {
    if (raw.empty() || raw.front() == '/' || raw.back() == '/') return false;
    std::size_t segment_start = 0;
    for (std::size_t i = 0; i <= raw.size(); ++i) {
        const char c = i == raw.size() ? '/' : raw[i];
        if (i < raw.size() && (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F)) return false;
        if (c != '/') continue;
        const std::string_view part = raw.substr(segment_start, i - segment_start);
        if (part.empty() || part == "." || part == "..") return false;
        segment_start = i + 1;
    }
    return true;
}

} // namespace

Result<std::string> normalize_path(std::string_view raw) {
    if (already_normalized(raw)) return std::string(raw);
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= raw.size(); ++i) {
        const bool end = i == raw.size();
        const char c = end ? '/' : raw[i];
        if (!end) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                return fail(ErrorCode::InvalidPath, "control character in path");
            }
            if (c == ':') return fail(ErrorCode::InvalidPath, "':' is not allowed in paths");
        }
        if (c != '/' && c != '\\') continue;
        const std::string_view part = raw.substr(begin, i - begin);
        begin = i + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) return fail(ErrorCode::InvalidPath, "path escapes the root: " + std::string(raw));
            parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    if (parts.empty()) return fail(ErrorCode::InvalidPath, "empty path");

    std::string result;
    for (const std::string_view part : parts) {
        if (!result.empty()) result += '/';
        result += part;
    }
    return result;
}

Result<AssetId> asset_id(std::string_view raw) {
    auto normalized = normalize_path(raw);
    if (!normalized) return std::unexpected(std::move(normalized.error()));
    return asset_id_of_normalized(*normalized);
}

std::string extension_of(std::string_view normalized_path) {
    const std::size_t slash = normalized_path.rfind('/');
    const std::string_view file = slash == std::string_view::npos ? normalized_path : normalized_path.substr(slash + 1);
    const std::size_t dot = file.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    std::string ext(file.substr(dot));
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

std::string_view directory_of(std::string_view normalized_path) noexcept {
    const std::size_t slash = normalized_path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : normalized_path.substr(0, slash);
}

} // namespace AssetSystem
