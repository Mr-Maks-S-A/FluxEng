#include <AssetSystem/AssetId.hpp>
#include <AssetSystem/Cook.hpp>
#include <AssetSystem/Loaders.hpp>

#include <algorithm>

namespace AssetSystem {

Result<CookReport> cook_directory(const std::filesystem::path& source, PackWriter& pack, const AssetLimits& limits) {
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) return fail(ErrorCode::NotFound, "not a directory: " + source.generic_string());

    auto owned = std::make_unique<DirectorySource>(source, limits.max_file_bytes);
    const DirectorySource& directory = *owned;
    VirtualFileSystem vfs; // glTF читает внешние .bin через него
    vfs.mount(std::move(owned));

    std::vector<std::string> files;
    directory.list(files);
    std::ranges::sort(files);

    CookReport report;
    for (const std::string& path : files) {
        const std::string ext = extension_of(path);
        if (ext == ".bin") {
            report.warnings.push_back("skipped " + path + " (glTF buffers are baked into .fmesh; rename if it is real data)");
            continue;
        }
        auto bytes = directory.read(path);
        if (!bytes) return fail(bytes.error().code, path + ": " + bytes.error().message);
        report.bytes_in += bytes->size();

        std::string out_path = path;
        Bytes out;
        if (ext == ".gltf" || ext == ".glb") {
            auto mesh = parse_gltf(*bytes, path, &vfs, limits);
            if (!mesh) return fail(mesh.error().code, path + ": " + mesh.error().message);
            out = serialize_mesh(*mesh);
            out_path = path.substr(0, path.rfind('.')) + ".fmesh";
            ++report.meshes_cooked;
        } else {
            out = std::move(*bytes);
        }
        report.bytes_out += out.size();
        if (!pack.add(out_path, std::move(out))) return fail(ErrorCode::Corrupt, "two sources cook to the same path: " + out_path);
        ++report.files;
    }
    return report;
}

} // namespace AssetSystem
