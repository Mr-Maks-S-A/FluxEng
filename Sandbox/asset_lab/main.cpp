/**
 * @file main.cpp
 * @brief AssetLab — путь ассета от исходника до экрана: glTF + PNG + JSON на диске → cook → пак → VFS → менеджер → GPU.
 *
 * Что показывает:
 *  - **Модуль движка в клиенте.** `AssetsModule` (RuntimeSystem::Module) добавляется в Game::configure(), живёт в Runtime
 *    и получает фоновые потоки JobSystem; игра берёт у него AssetManager.
 *  - **Cooker.** По умолчанию исходники превращаются в пак `.fxpk` (glTF → бинарный `.fmesh`); `--raw` монтирует исходный
 *    каталог и разбирает glTF при каждой загрузке — разницу видно в таймингах на экране.
 *  - **Асинхронная загрузка.** Разбор идёт в рабочих потоках, а в GPU ассеты попадают в главном потоке из `pump()`.
 *  - **Ошибки.** Несуществующий файл загружается «успешно» с состоянием Failed — причина показана красным, игра не падает.
 *  - **Перезагрузка.** R — выбросить ассеты из кеша и загрузить заново (так будет работать горячая перезагрузка).
 *
 * Управление: R — перезагрузить, пробел — пауза вращения (клавиша паузы App отключена), Esc — выход.
 * Аргументы: `--raw` (без cook), общие `--ticks`, `--threads`, `--backend`, `--screenshot` (см. Core::App).
 */

#include <AssetSystem/AssetSystem.hpp>
#include <Core/Core.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <vector>

namespace as = AssetSystem;
namespace fs = std::filesystem;
namespace rs = RuntimeSystem;
using namespace RendererSystem;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

void write_file(const fs::path& path, const void* data, std::size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

// ===================================================================== модуль ассетов

/// Модуль движка: готовит источники, монтирует их и держит AssetManager. Ничего не знает об окне и GPU.
class AssetsModule final : public rs::Module {
public:
    explicit AssetsModule(bool raw) : m_raw(raw) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "Assets"; }

    void init(rs::Runtime& runtime) override {
        const fs::path root = fs::temp_directory_path() / "flux_asset_lab";
        const fs::path source = root / "src";
        fs::remove_all(root);

        // 1. Исходники, как их отдал бы художник: glTF с внешним .bin, PNG, JSON данных.
        const auto cube = as::procedural::make_gltf_box({.embedded = false, .translation = {0, 0, 0}, .scale = {1.4f, 1.4f, 1.4f},
                                                         .texture_uri = "../textures/rune.png", .buffer_uri = "rune_cube.bin"});
        write_file(source / "models/rune_cube.gltf", cube.json.data(), cube.json.size());
        write_file(source / "models/rune_cube.bin", cube.bin.data(), cube.bin.size());
        const auto png = Image::checkerboard(256, 256, 32, Color::from_rgba(0xE8C547FF), Color::from_rgba(0x6A3FA0FF)).encode_png();
        write_file(source / "textures/rune.png", png.data(), png.size());
        const std::string spells = R"({"fireball":{"damage":12,"mana":8},"frost":{"damage":7,"mana":5}})";
        write_file(source / "data/spells.json", spells.data(), spells.size());

        // 2. Либо прямо из каталога (--raw), либо cook → пак.
        if (m_raw) {
            m_vfs.mount(std::make_unique<as::DirectorySource>(source));
            m_summary = "режим: исходники с диска (glTF разбирается при загрузке)";
        } else {
            const auto cook_start = Clock::now();
            as::PackWriter pack;
            const auto report = as::cook_directory(source, pack);
            if (!report) throw std::runtime_error("cook: " + report.error().what());
            const fs::path pack_path = root / "lab.fxpk";
            if (const auto written = pack.write_file(pack_path); !written) throw std::runtime_error("pack: " + written.error().what());
            auto mounted = as::PackSource::open_file(pack_path);
            if (!mounted) throw std::runtime_error("pack: " + mounted.error().what());
            m_vfs.mount(std::move(*mounted));
            m_summary = std::format("режим: пак  (cook {:.1f} мс, {} файлов, {} сеток, {} → {} байт)", ms_since(cook_start), report->files,
                                    report->meshes_cooked, report->bytes_in, report->bytes_out);
        }
        m_assets = std::make_unique<as::AssetManager>(m_vfs, &runtime.jobs());
        std::println("assets: {}", m_summary);
    }

    void shutdown(rs::Runtime&) noexcept override {
        m_assets.reset(); // дожидается задач в полёте, пока Scheduler ещё жив
        std::error_code ec;
        fs::remove_all(fs::temp_directory_path() / "flux_asset_lab", ec);
    }

    [[nodiscard]] as::AssetManager& assets() noexcept { return *m_assets; }
    [[nodiscard]] bool raw() const noexcept { return m_raw; }
    [[nodiscard]] const std::string& summary() const noexcept { return m_summary; }

private:
    bool m_raw;
    as::VirtualFileSystem m_vfs;
    std::unique_ptr<as::AssetManager> m_assets;
    std::string m_summary;
};

// ===================================================================== игра

MeshData to_mesh_data(const as::MeshAsset& asset) {
    MeshData mesh;
    mesh.vertices.reserve(asset.vertices.size());
    for (const as::MeshVertex& v : asset.vertices) {
        mesh.vertices.push_back(Vertex3D{.position = {v.position[0], v.position[1], v.position[2]},
                                         .normal = {v.normal[0], v.normal[1], v.normal[2]},
                                         .uv = {v.uv[0], v.uv[1]},
                                         .color = Color{static_cast<std::uint8_t>(v.rgba & 0xFF), static_cast<std::uint8_t>((v.rgba >> 8) & 0xFF),
                                                        static_cast<std::uint8_t>((v.rgba >> 16) & 0xFF), static_cast<std::uint8_t>(v.rgba >> 24)}});
    }
    mesh.indices = asset.indices;
    return mesh;
}

Image to_image(const as::ImageAsset& asset) {
    Image image(asset.width, asset.height);
    auto pixels = image.pixels();
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = Color{asset.rgba[i * 4], asset.rgba[i * 4 + 1], asset.rgba[i * 4 + 2], asset.rgba[i * 4 + 3]};
    }
    return image;
}

/// Строка состояния одного ассета для HUD.
struct Line {
    std::string label;
    std::string text;
    bool ok = false;
    bool failed = false;
};

class AssetLab final : public Core::Game {
public:
    void configure(Core::App& app) override {
        const auto& args = app.config().extra_args;
        m_raw = std::ranges::find(args, "--raw") != args.end();
        m_module = &app.add_module<AssetsModule>(m_raw);
    }

    void setup(Core::App& app) override {
        m_device = &app.device();
        request_all();
    }

    void frame(Core::App& app, float) override {
        m_module->assets().pump(); // здесь, в главном потоке, выполняются on_done: заливка в GPU
        const auto& input = app.window().input();
        if (input.pressed(GLFW_KEY_R)) reload();
        if (input.pressed(GLFW_KEY_SPACE)) m_spin = !m_spin;
    }

    void tick(Core::App& app) override {
        if (m_spin) m_angle += 0.6f * app.tick_seconds();
    }

    void render_3d(Core::App& app, Renderer3D& r) override {
        r.begin(Camera3D::orbit({0, 0, 0}, m_angle, 0.45f, 4.2f, app.camera().viewport));
        if (m_mesh.valid()) {
            Material material;
            if (m_texture.valid()) material.texture = &m_texture;
            r.draw(m_mesh, glm::mat4{1.0f}, material);
        }
        r.end();
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        float y = 14.0f;
        const auto line = [&](const std::string& text, Color color) {
            r.draw_text(app.ui_font(), text, {14.0f, y}, {.size = 20.0f, .color = color, .shadow = Color{0, 0, 0, 200}});
            y += 26.0f;
        };
        line("AssetLab — cook → пак → VFS → менеджер → GPU", Colors::white);
        line(m_module->summary(), Color::from_rgba(0x9AB7D3FF));
        for (const Line& l : m_lines) line(l.label + ": " + l.text, l.failed ? Color::from_rgba(0xFF6B6BFF) : (l.ok ? Color::from_rgba(0x8FE388FF) : Colors::yellow));
        const as::AssetStats s = m_module->assets().stats();
        line(std::format("менеджер: загрузок {} (ошибок {}), из кеша {}, прочитано {} КБ, в полёте {}", s.loads_ok, s.loads_failed, s.cache_hits,
                         s.bytes_read / 1024, s.in_flight), Color::from_rgba(0x9AB7D3FF));
        line("R — перезагрузить с нуля   Пробел — пауза вращения", Color::from_rgba(0x8892A0FF));
    }

    void shutdown(Core::App&) override {
        const auto count = [this](const char* label) {
            for (const Line& l : m_lines) {
                if (l.label == label) return l.ok ? 1 : 0;
            }
            return 0;
        };
        const int failed_as_expected = [&] {
            for (const Line& l : m_lines) {
                if (l.label == "textures/missing.png") return l.failed ? 1 : 0;
            }
            return 0;
        }();
        std::println("asset_lab: mesh={} texture={} spells={} failed_as_expected={} reloads={}", count("mesh"), count("texture"), count("spells"),
                     failed_as_expected, m_reloads);
    }

private:
    Line& line_for(const std::string& label) {
        for (Line& l : m_lines) {
            if (l.label == label) return l;
        }
        m_lines.push_back({label, "ждём…", false, false});
        return m_lines.back();
    }

    void request_all() {
        auto& assets = m_module->assets();
        const auto start = Clock::now();
        m_lines.clear();
        m_mesh = Mesh{};
        m_texture = Texture{};

        const std::string mesh_path = m_raw ? "models/rune_cube.gltf" : "models/rune_cube.fmesh";
        m_handles_mesh = assets.load<as::MeshAsset>(mesh_path);
        line_for("mesh");
        assets.on_done<as::MeshAsset>(m_handles_mesh, [this, start](const as::Handle<as::MeshAsset>& h) {
            Line& l = line_for("mesh");
            if (!h.ready()) { l.failed = true; l.text = h.error()->what(); return; }
            m_mesh = Mesh::create(*m_device, to_mesh_data(*h.get()));
            l.ok = true;
            l.text = std::format("{} — {} вершин, {} треугольников, готово через {:.1f} мс (в GPU {} байт)", h.path(), h.get()->vertices.size(),
                                 h.get()->triangle_count(), ms_since(start), m_mesh.gpu_bytes());
            // Текстура указана внутри сетки: сначала сетка, потом то, на что она ссылается.
            request_texture(h.get()->base_color_texture, start);
        });

        m_handles_spells = assets.load<as::TextAsset>("data/spells.json");
        line_for("spells");
        assets.on_done<as::TextAsset>(m_handles_spells, [this, start](const as::Handle<as::TextAsset>& h) {
            Line& l = line_for("spells");
            if (!h.ready()) { l.failed = true; l.text = h.error()->what(); return; }
            l.ok = true;
            l.text = std::format("{} — {} байт JSON, готово через {:.1f} мс", h.path(), h.get()->text.size(), ms_since(start));
        });

        // Намеренная ошибка: показывает, что отсутствие файла — состояние, а не исключение.
        m_handles_missing = assets.load<as::ImageAsset>("textures/missing.png");
        assets.on_done<as::ImageAsset>(m_handles_missing, [this](const as::Handle<as::ImageAsset>& h) {
            Line& l = line_for("textures/missing.png");
            l.failed = !h.ready();
            l.ok = h.ready();
            l.text = h.ready() ? "неожиданно загрузился" : h.error()->what();
        });
    }

    void request_texture(const std::string& path, Clock::time_point start) {
        if (path.empty()) return;
        line_for("texture");
        m_handles_texture = m_module->assets().load<as::ImageAsset>(path);
        m_module->assets().on_done<as::ImageAsset>(m_handles_texture, [this, start](const as::Handle<as::ImageAsset>& h) {
            Line& l = line_for("texture");
            if (!h.ready()) { l.failed = true; l.text = h.error()->what(); return; }
            m_texture = Texture::create(*m_device, to_image(*h.get()), {.filter = TextureFilter::Linear, .wrap = TextureWrap::Repeat, .mipmaps = true});
            l.ok = true;
            l.text = std::format("{} — {}x{}, готово через {:.1f} мс", h.path(), h.get()->width, h.get()->height, ms_since(start));
        });
    }

    void reload() {
        auto& assets = m_module->assets();
        assets.evict<as::MeshAsset>(m_handles_mesh.path());
        assets.evict<as::ImageAsset>(m_handles_texture.path());
        assets.evict<as::TextAsset>(m_handles_spells.path());
        assets.evict<as::ImageAsset>("textures/missing.png");
        ++m_reloads;
        request_all();
    }

    AssetsModule* m_module = nullptr;
    RHI::Device* m_device = nullptr;
    bool m_raw = false;
    bool m_spin = true;
    float m_angle = 0.4f;
    int m_reloads = 0;

    std::vector<Line> m_lines;
    as::Handle<as::MeshAsset> m_handles_mesh;
    as::Handle<as::ImageAsset> m_handles_texture;
    as::Handle<as::TextAsset> m_handles_spells;
    as::Handle<as::ImageAsset> m_handles_missing;
    Mesh m_mesh;
    Texture m_texture;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<AssetLab>({.title = "AssetLab", .ticks_per_second = 60.0, .pause_key = 0, .camera_controls = false,
                                .clear_rgba = 0x101018FF},
                               argc, argv);
}
