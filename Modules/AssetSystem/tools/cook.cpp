/**
 * @file cook.cpp
 * @brief flux_cook <исходный-каталог> <выходной.fxpk> — собирает пак для рантайма (glTF → .fmesh, остальное как есть).
 */

#include <AssetSystem/AssetSystem.hpp>

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <source-dir> <out.fxpk>\n", argv[0]);
        return 2;
    }
    AssetSystem::PackWriter pack;
    const auto report = AssetSystem::cook_directory(argv[1], pack);
    if (!report) {
        std::fprintf(stderr, "cook failed: %s\n", report.error().what().c_str());
        return 1;
    }
    for (const std::string& warning : report->warnings) std::fprintf(stderr, "warning: %s\n", warning.c_str());
    if (const auto written = pack.write_file(argv[2]); !written) {
        std::fprintf(stderr, "write failed: %s\n", written.error().what().c_str());
        return 1;
    }
    std::printf("cooked %zu files (%zu meshes), %zu -> %zu bytes -> %s\n", report->files, report->meshes_cooked, report->bytes_in,
                report->bytes_out, argv[2]);
    return 0;
}
