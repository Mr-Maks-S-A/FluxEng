#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Text/Font.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

// stb_truetype (запекание глифов), stb_rect_pack (упаковка атласа) и stb_easy_font (встроенный шрифт)
// собираются здесь и только здесь.
#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wcast-qual"
#    pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#include <stb_rect_pack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>
#include <stb_easy_font.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace RendererSystem {

std::uint32_t next_codepoint(std::string_view text, std::size_t& index) noexcept {
    constexpr std::uint32_t replacement = 0xFFFD;
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    const std::uint8_t lead = byte(index++);
    if (lead < 0x80) {
        return lead;
    }
    int extra = 0;
    std::uint32_t code = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        code = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        code = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        code = lead & 0x07u;
    } else {
        return replacement; // байт продолжения или запрещённый байт на месте первого
    }
    for (int i = 0; i < extra; ++i) {
        if (index >= text.size() || (byte(index) & 0xC0) != 0x80) {
            return replacement;
        }
        code = (code << 6) | (byte(index++) & 0x3Fu);
    }
    return code;
}

namespace {

std::uint64_t pair_key(std::uint32_t left, std::uint32_t right) noexcept {
    return (std::uint64_t{left} << 32) | right;
}

std::expected<std::vector<std::byte>, std::string> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("cannot open font '{}'", path.string()));
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    std::ranges::transform(bytes, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

} // namespace

std::expected<Font, std::string> Font::load(const std::filesystem::path& path, const FontConfig& config) {
    auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto font = from_memory(*bytes, config);
    if (!font) {
        return std::unexpected(std::format("'{}': {}", path.string(), font.error()));
    }
    font->m_source = path.string();
    return font;
}

std::expected<Font, std::string> Font::from_memory(std::span<const std::byte> ttf, const FontConfig& config) {
    if (ttf.empty()) {
        return std::unexpected(std::string("font data is empty"));
    }
    if (config.pixel_height <= 0.0f) {
        return std::unexpected(std::string("font pixel height must be positive"));
    }
    const auto* data = reinterpret_cast<const unsigned char*>(ttf.data());
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    stbtt_fontinfo info{};
    if (offset < 0 || stbtt_InitFont(&info, data, offset) == 0) {
        return std::unexpected(std::string("not a TrueType/OpenType font"));
    }

    // Только символы, которые в шрифте есть: отсутствующие нарисуются запасным глифом.
    std::vector<int> codepoints;
    std::vector<int> glyph_indices;
    for (const CodepointRange& range : config.ranges) {
        for (std::uint32_t cp = range.first; cp <= range.last; ++cp) {
            const int glyph = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
            if (glyph != 0 && std::ranges::find(codepoints, static_cast<int>(cp)) == codepoints.end()) {
                codepoints.push_back(static_cast<int>(cp));
                glyph_indices.push_back(glyph);
            }
        }
    }
    if (codepoints.empty()) {
        return std::unexpected(std::string("font has none of the requested characters"));
    }

    std::vector<stbtt_packedchar> packed(codepoints.size());
    std::vector<unsigned char> coverage;
    int atlas_size = 0;
    for (int size = 512; size <= std::max(config.max_atlas_size, 512); size *= 2) {
        coverage.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0);
        stbtt_pack_context context{};
        if (stbtt_PackBegin(&context, coverage.data(), size, size, 0, std::max(config.padding, 0), nullptr) == 0) {
            return std::unexpected(std::string("stbtt_PackBegin failed"));
        }
        const auto oversample = static_cast<unsigned>(std::clamp(config.oversample, 1, 4));
        stbtt_PackSetOversampling(&context, oversample, oversample);
        stbtt_pack_range range{};
        range.font_size = config.pixel_height;
        range.array_of_unicode_codepoints = codepoints.data();
        range.num_chars = static_cast<int>(codepoints.size());
        range.chardata_for_range = packed.data();
        const int ok = stbtt_PackFontRanges(&context, data, 0, &range, 1);
        stbtt_PackEnd(&context);
        if (ok != 0) {
            atlas_size = size;
            break;
        }
    }
    if (atlas_size == 0) {
        return std::unexpected(std::format("{} glyphs do not fit into a {}x{} atlas", codepoints.size(),
                                           config.max_atlas_size, config.max_atlas_size));
    }

    Font font;
    font.m_source = "memory";
    font.m_pixel_height = config.pixel_height;
    const float scale = stbtt_ScaleForPixelHeight(&info, config.pixel_height);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    font.m_ascent = static_cast<float>(ascent) * scale;
    font.m_descent = static_cast<float>(descent) * scale;
    font.m_line_height = static_cast<float>(ascent - descent + line_gap) * scale;

    font.m_atlas = Image(atlas_size, atlas_size, Colors::transparent);
    std::span<Color> pixels = font.m_atlas.pixels();
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        pixels[i] = Color{255, 255, 255, coverage[i]};
    }

    font.m_glyphs.reserve(codepoints.size());
    for (std::size_t i = 0; i < codepoints.size(); ++i) {
        const stbtt_packedchar& pc = packed[i];
        font.m_glyphs.push_back(Glyph{
            .codepoint = static_cast<std::uint32_t>(codepoints[i]),
            .atlas = Rect{{static_cast<float>(pc.x0), static_cast<float>(pc.y0)},
                          {static_cast<float>(pc.x1 - pc.x0), static_cast<float>(pc.y1 - pc.y0)}},
            .offset = {pc.xoff, pc.yoff},
            .size = {pc.xoff2 - pc.xoff, pc.yoff2 - pc.yoff},
            .advance = pc.xadvance,
        });
    }

    // Кернинг: для всех пар запечённых глифов (таблица kern или GPOS — что есть в шрифте).
    if (stbtt_GetKerningTableLength(&info) > 0 || info.gpos != 0) {
        for (std::size_t a = 0; a < codepoints.size(); ++a) {
            for (std::size_t b = 0; b < codepoints.size(); ++b) {
                const int kern = stbtt_GetGlyphKernAdvance(&info, glyph_indices[a], glyph_indices[b]);
                if (kern != 0) {
                    font.m_kerning.emplace(pair_key(static_cast<std::uint32_t>(codepoints[a]), static_cast<std::uint32_t>(codepoints[b])),
                                           static_cast<float>(kern) * scale);
                }
            }
        }
    }

    font.index_glyphs();
    return font;
}

Font Font::builtin(int scale) {
    // stb_easy_font рисует символы прямоугольниками в единицах «пикселя шрифта»:
    // заглавные занимают строки 0…6 (базовая линия — 7), хвосты строчных доходят до 9, строка — 12.
    const int s = std::max(scale, 1);
    constexpr int cell_units = 10;
    constexpr int baseline_units = 7;
    constexpr int atlas_width = 512;
    constexpr int padding = 1;

    struct EasyVertex {
        float x, y, z;
        unsigned char color[4];
    };
    static_assert(sizeof(EasyVertex) == 16);

    Font font;
    font.m_source = "builtin";
    font.m_pixel_height = static_cast<float>(12 * s);
    font.m_ascent = static_cast<float>(8 * s);
    font.m_descent = static_cast<float>(-4 * s);
    font.m_line_height = static_cast<float>(12 * s);

    // Сначала раскладка ячеек по строкам атласа, потом рисование.
    struct Cell {
        char character;
        int x, y, width;
    };
    std::vector<Cell> cells;
    int pen_x = padding;
    int pen_y = padding;
    const int cell_height = cell_units * s;
    for (char c = 32; c < 127; ++c) {
        char text[2] = {c, '\0'};
        const int width = stb_easy_font_width(text) * s;
        if (pen_x + width + padding > atlas_width) {
            pen_x = padding;
            pen_y += cell_height + padding;
        }
        cells.push_back({c, pen_x, pen_y, width});
        pen_x += width + padding;
    }
    int atlas_height = 1;
    while (atlas_height < pen_y + cell_height + padding) {
        atlas_height *= 2;
    }
    font.m_atlas = Image(atlas_width, atlas_height, Colors::transparent);

    std::array<EasyVertex, 256> vertices{};
    for (const Cell& cell : cells) {
        char text[2] = {cell.character, '\0'};
        const int quads = stb_easy_font_print(0.0f, 0.0f, text, nullptr, vertices.data(), static_cast<int>(sizeof(vertices)));
        for (int q = 0; q < quads; ++q) {
            const EasyVertex& top_left = vertices[static_cast<std::size_t>(q) * 4];
            const EasyVertex& bottom_right = vertices[static_cast<std::size_t>(q) * 4 + 2];
            font.m_atlas.fill_rect(cell.x + static_cast<int>(top_left.x) * s, cell.y + static_cast<int>(top_left.y) * s,
                                   static_cast<int>(bottom_right.x - top_left.x) * s,
                                   static_cast<int>(bottom_right.y - top_left.y) * s, Colors::white);
        }
        font.m_glyphs.push_back(Glyph{
            .codepoint = static_cast<std::uint32_t>(cell.character),
            .atlas = Rect{{static_cast<float>(cell.x), static_cast<float>(cell.y)},
                          {static_cast<float>(cell.width), static_cast<float>(cell_height)}},
            .offset = {0.0f, static_cast<float>(-baseline_units * s)},
            // Пустой глиф (пробел) — без четырёхугольника: раскладка его не рисует.
            .size = quads > 0 ? glm::vec2{static_cast<float>(cell.width), static_cast<float>(cell_height)} : glm::vec2{0.0f},
            .advance = static_cast<float>(cell.width),
        });
    }
    font.index_glyphs();
    return font;
}

std::optional<std::filesystem::path> Font::find_system_font(bool bold) {
    if (const char* forced = std::getenv(bold ? "FLUX_FONT_BOLD" : "FLUX_FONT"); forced != nullptr && *forced != '\0') {
        return std::filesystem::path(forced);
    }
    static constexpr std::string_view regular[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
    };
    static constexpr std::string_view heavy[] = {
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "C:/Windows/Fonts/segoeuib.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/Library/Fonts/Arial Bold.ttf",
    };
    for (const std::string_view candidate : bold ? std::span<const std::string_view>(heavy) : std::span<const std::string_view>(regular)) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return std::filesystem::path(candidate);
        }
    }
    return std::nullopt;
}

std::expected<Font, std::string> Font::load_system(const FontConfig& config, bool bold) {
    const auto path = find_system_font(bold);
    if (!path) {
        return std::unexpected(std::string("no known system font found (set FLUX_FONT to a .ttf file)"));
    }
    return load(*path, config);
}

void Font::index_glyphs() {
    std::ranges::sort(m_glyphs, {}, &Glyph::codepoint);
    m_ascii.fill(0);
    m_fallback = 0;
    for (std::size_t i = 0; i < m_glyphs.size(); ++i) {
        const std::uint32_t cp = m_glyphs[i].codepoint;
        if (cp < m_ascii.size()) {
            m_ascii[cp] = static_cast<std::int32_t>(i + 1);
        }
        if (cp == '?') {
            m_fallback = i;
        }
    }
}

const Glyph* Font::find(std::uint32_t codepoint) const noexcept {
    if (codepoint < m_ascii.size()) {
        const std::int32_t slot = m_ascii[codepoint];
        return slot > 0 ? &m_glyphs[static_cast<std::size_t>(slot - 1)] : nullptr;
    }
    const auto it = std::ranges::lower_bound(m_glyphs, codepoint, {}, &Glyph::codepoint);
    return it != m_glyphs.end() && it->codepoint == codepoint ? &*it : nullptr;
}

const Glyph& Font::glyph(std::uint32_t codepoint) const noexcept {
    const Glyph* found = find(codepoint);
    return found != nullptr ? *found : m_glyphs[m_fallback];
}

float Font::kerning(std::uint32_t left, std::uint32_t right) const noexcept {
    if (m_kerning.empty()) {
        return 0.0f;
    }
    const auto it = m_kerning.find(pair_key(left, right));
    return it != m_kerning.end() ? it->second : 0.0f;
}

float Font::scale_for(const TextLayoutOptions& options) const noexcept {
    return options.size > 0.0f ? options.size / m_pixel_height : 1.0f;
}

float Font::run_width(std::string_view text, float scale) const noexcept {
    float width = 0.0f;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = next_codepoint(text, i);
        width += (glyph(cp).advance + (previous != 0 ? kerning(previous, cp) : 0.0f)) * scale;
        previous = cp;
    }
    return width;
}

void Font::break_lines(std::string_view text, float scale, float max_width, std::vector<Line>& lines) const {
    std::size_t paragraph = 0;
    while (true) {
        const std::size_t newline = text.find('\n', paragraph);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        if (max_width <= 0.0f) {
            lines.push_back({paragraph, end, run_width(text.substr(paragraph, end - paragraph), scale)});
        } else {
            // Перенос по словам: слово, которое не влезает в пустую строку, остаётся целиком (выходит за край).
            Line line{paragraph, paragraph, 0.0f};
            std::size_t pos = paragraph;
            while (pos < end) {
                std::size_t word_begin = pos;
                while (word_begin < end && text[word_begin] == ' ') ++word_begin;
                if (word_begin == end) break;
                std::size_t word_end = word_begin;
                while (word_end < end && text[word_end] != ' ') ++word_end;
                const float candidate = run_width(text.substr(line.begin, word_end - line.begin), scale);
                if (line.end > line.begin && candidate > max_width) {
                    lines.push_back(line);
                    line = {word_begin, word_end, run_width(text.substr(word_begin, word_end - word_begin), scale)};
                } else {
                    if (line.end == line.begin) line.begin = word_begin; // ведущие пробелы строки не рисуем
                    line.end = word_end;
                    line.width = run_width(text.substr(line.begin, word_end - line.begin), scale);
                }
                pos = word_end;
            }
            lines.push_back(line);
        }
        if (newline == std::string_view::npos) break;
        paragraph = newline + 1;
    }
}

glm::vec2 Font::measure(std::string_view text, const TextLayoutOptions& options) const {
    std::vector<Line> lines;
    const float scale = scale_for(options);
    break_lines(text, scale, options.max_width, lines);
    float width = 0.0f;
    for (const Line& line : lines) width = std::max(width, line.width);
    const float advance = m_line_height * scale * options.line_spacing;
    const float height = (m_ascent - m_descent) * scale + advance * static_cast<float>(lines.size() - 1);
    return {width, height};
}

glm::vec2 Font::layout(std::string_view text, const TextLayoutOptions& options, std::vector<GlyphQuad>& out) const {
    std::vector<Line> lines;
    const float scale = scale_for(options);
    break_lines(text, scale, options.max_width, lines);

    float widest = 0.0f;
    for (const Line& line : lines) widest = std::max(widest, line.width);
    const float block = options.max_width > 0.0f ? std::max(options.max_width, widest) : widest;
    const float advance = m_line_height * scale * options.line_spacing;
    const glm::vec2 atlas_size{static_cast<float>(m_atlas.width()), static_cast<float>(m_atlas.height())};

    for (std::size_t row = 0; row < lines.size(); ++row) {
        const Line& line = lines[row];
        float pen = 0.0f;
        if (options.align == TextAlign::Center) pen = (block - line.width) * 0.5f;
        if (options.align == TextAlign::Right) pen = block - line.width;
        const float baseline = m_ascent * scale + advance * static_cast<float>(row);

        std::uint32_t previous = 0;
        for (std::size_t i = line.begin; i < line.end;) {
            const std::uint32_t cp = next_codepoint(text, i);
            const Glyph& g = glyph(cp);
            if (previous != 0) pen += kerning(previous, cp) * scale;
            if (g.size.x > 0.0f && g.size.y > 0.0f) {
                out.push_back(GlyphQuad{
                    .rect = Rect{{pen + g.offset.x * scale, baseline + g.offset.y * scale}, g.size * scale},
                    .uv = UvRect::from_pixels(g.atlas, atlas_size),
                });
            }
            pen += g.advance * scale;
            previous = cp;
        }
    }
    const float height = (m_ascent - m_descent) * scale + advance * static_cast<float>(lines.size() - 1);
    return {block, height};
}

} // namespace RendererSystem
