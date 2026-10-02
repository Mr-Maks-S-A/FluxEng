#pragma once
/**
 * @file Font.hpp
 * @brief Шрифт как атлас глифов (stb_truetype + stb_rect_pack) и раскладка текста — без OpenGL.
 *
 * Font только готовит данные: атлас-Image и прямоугольники глифов. Рисует Renderer2D::draw_text().
 * Поэтому раскладку (перенос строк, выравнивание, кернинг) можно проверять в тестах без видеокарты.
 */

#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Image.hpp>

#include <glm/vec2.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RendererSystem {

/**
 * @brief Следующий символ UTF-8 из `text`, начиная с байта `index` (index сдвигается за символ).
 * @return Код символа; U+FFFD для испорченной последовательности.
 */
[[nodiscard]] std::uint32_t next_codepoint(std::string_view text, std::size_t& index) noexcept;

/// @brief Диапазон кодов символов `[first, last]`.
struct CodepointRange {
    std::uint32_t first = 0; ///< Первый код.
    std::uint32_t last = 0;  ///< Последний код (включительно).
};

/// @brief Готовые диапазоны.
namespace CodepointRanges {
inline constexpr CodepointRange ascii{0x20, 0x7E};        ///< Латиница, цифры, знаки.
inline constexpr CodepointRange latin1{0xA0, 0xFF};       ///< «», ©, °, ×, ±, …
inline constexpr CodepointRange cyrillic{0x400, 0x45F};   ///< Русский алфавит (и ё).
inline constexpr CodepointRange punctuation{0x2010, 0x2027}; ///< Тире, кавычки, многоточие, •.
inline constexpr CodepointRange scripts{0x2070, 0x209F};  ///< ⁰¹²… и ₀₁₂… — формулы (H₂O, x²).
inline constexpr CodepointRange arrows{0x2190, 0x2195};   ///< ← ↑ → ↓.
inline constexpr CodepointRange math{0x2200, 0x22FF};     ///< − ≡ ≈ ≠ ≤ ∞ … (математические операторы)
inline constexpr CodepointRange geometry{0x25A0, 0x25FF}; ///< ■ □ ▲ ● ◯ ◆ … (если есть в шрифте).
inline constexpr CodepointRange symbols{0x2600, 0x266F};  ///< ★ ☀ ♥ ♦ ♣ … (если есть в шрифте).
} // namespace CodepointRanges

/// @brief Параметры запекания TTF-шрифта.
struct FontConfig {
    float pixel_height = 32.0f;  ///< Высота строки (ascent − descent) в пикселях атласа.
    std::vector<CodepointRange> ranges{CodepointRanges::ascii, CodepointRanges::latin1, CodepointRanges::cyrillic,
                                       CodepointRanges::punctuation, CodepointRanges::scripts, CodepointRanges::arrows, CodepointRanges::math,
                                       CodepointRanges::geometry, CodepointRanges::symbols}; ///< Какие символы запечь (отсутствующие в шрифте пропускаются).
    int oversample = 2;          ///< Передискретизация по X и Y (чётче при уменьшении и дробных позициях).
    int padding = 1;             ///< Пиксели между глифами в атласе.
    int max_atlas_size = 4096;   ///< Атлас растёт от 512 до этого размера, пока глифы не поместятся.
};

/**
 * @brief Глиф в атласе. Координаты — пиксели атласа при `Font::pixel_height()`.
 */
struct Glyph {
    std::uint32_t codepoint = 0; ///< Код символа.
    Rect atlas{};                ///< Область в атласе.
    glm::vec2 offset{0.0f};      ///< Левый верх четырёхугольника относительно пера на базовой линии (Y вниз).
    glm::vec2 size{0.0f};        ///< Размер четырёхугольника (0 — пробел).
    float advance = 0.0f;        ///< Сдвиг пера после символа.
};

/// @brief Выравнивание строк.
enum class TextAlign : std::uint8_t { Left, Center, Right };

/// @brief Параметры раскладки текста.
struct TextLayoutOptions {
    float size = 0.0f;               ///< Кегль: высота (ascent − descent) в пикселях экрана; 0 — как в атласе.
    TextAlign align = TextAlign::Left; ///< Выравнивание строк внутри блока.
    float max_width = 0.0f;          ///< Перенос по словам при превышении ширины; 0 — без переноса.
    float line_spacing = 1.0f;       ///< Множитель межстрочного интервала.
};

/// @brief Четырёхугольник глифа после раскладки: куда рисовать и откуда брать.
struct GlyphQuad {
    Rect rect{};  ///< Прямоугольник на экране; (0, 0) — левый верх блока текста.
    UvRect uv{};  ///< Область атласа.
};

/**
 * @brief Шрифт: атлас глифов, метрики и кернинг.
 *
 * Источники:
 * - Font::load() / Font::from_memory() — TTF/OTF через stb_truetype, упаковка stb_rect_pack;
 * - Font::builtin() — встроенный растровый ASCII-шрифт (stb_easy_font): работает без файлов,
 *   годится для отладки, тестов и как запасной вариант.
 *
 * @code
 * Font font = Font::load_system().value_or(Font::builtin());
 * FontHandle ui = renderer.add_font(std::move(font));
 * renderer.draw_text(ui, "Ход 3 — мана 3/3", {16, 16}, {.size = 24, .color = Colors::yellow});
 * @endcode
 */
class Font {
public:
    /// @brief Загружает TTF/OTF из файла и запекает атлас.
    [[nodiscard]] static std::expected<Font, std::string> load(const std::filesystem::path& path, const FontConfig& config = {});
    /// @brief Запекает атлас из данных TTF в памяти (данные после этого не нужны).
    [[nodiscard]] static std::expected<Font, std::string> from_memory(std::span<const std::byte> ttf, const FontConfig& config = {});
    /// @brief Встроенный ASCII-шрифт; `scale` — во сколько раз увеличить (строка = 12·scale пикселей).
    [[nodiscard]] static Font builtin(int scale = 2);

    /**
     * @brief Ищет распространённый TTF с кириллицей в системе (DejaVu, Noto, Liberation, Arial, Segoe…).
     *
     * Переменная окружения `FLUX_FONT` (или `FLUX_FONT_BOLD` для `bold`) задаёт путь явно.
     */
    [[nodiscard]] static std::optional<std::filesystem::path> find_system_font(bool bold = false);
    /// @brief find_system_font() + load().
    [[nodiscard]] static std::expected<Font, std::string> load_system(const FontConfig& config = {}, bool bold = false);

    /// @brief Атлас: белые пиксели, покрытие — в альфе.
    [[nodiscard]] const Image& atlas() const noexcept { return m_atlas; }
    /// @brief Высота строки, для которой запечён атлас.
    [[nodiscard]] float pixel_height() const noexcept { return m_pixel_height; }
    /// @brief Подъём над базовой линией (> 0).
    [[nodiscard]] float ascent() const noexcept { return m_ascent; }
    /// @brief Спуск под базовую линию (< 0).
    [[nodiscard]] float descent() const noexcept { return m_descent; }
    /// @brief Расстояние между базовыми линиями.
    [[nodiscard]] float line_height() const noexcept { return m_line_height; }
    /// @brief Количество глифов.
    [[nodiscard]] std::size_t glyph_count() const noexcept { return m_glyphs.size(); }
    /// @brief Откуда шрифт (путь или "builtin").
    [[nodiscard]] const std::string& source() const noexcept { return m_source; }

    /// @brief Глиф символа или nullptr.
    [[nodiscard]] const Glyph* find(std::uint32_t codepoint) const noexcept;
    /// @brief Глиф символа или запасной глиф ('?') для отсутствующих.
    [[nodiscard]] const Glyph& glyph(std::uint32_t codepoint) const noexcept;
    /// @brief Поправка расстояния между парой символов (пиксели атласа).
    [[nodiscard]] float kerning(std::uint32_t left, std::uint32_t right) const noexcept;

    /// @brief Размер блока текста.
    [[nodiscard]] glm::vec2 measure(std::string_view text, const TextLayoutOptions& options = {}) const;

    /**
     * @brief Раскладывает текст: дописывает в `out` четырёхугольники глифов, (0, 0) — левый верх блока.
     * @return Размер блока.
     */
    glm::vec2 layout(std::string_view text, const TextLayoutOptions& options, std::vector<GlyphQuad>& out) const;

private:
    struct Line {
        std::size_t begin = 0;
        std::size_t end = 0;
        float width = 0.0f;
    };

    Font() = default;
    void index_glyphs();
    [[nodiscard]] float scale_for(const TextLayoutOptions& options) const noexcept;
    [[nodiscard]] float run_width(std::string_view text, float scale) const noexcept;
    void break_lines(std::string_view text, float scale, float max_width, std::vector<Line>& lines) const;

    Image m_atlas;
    std::vector<Glyph> m_glyphs;                      ///< По возрастанию кода.
    std::array<std::int32_t, 128> m_ascii{};          ///< Индекс глифа ASCII + 1 (0 — нет).
    std::unordered_map<std::uint64_t, float> m_kerning; ///< (левый << 32 | правый) → поправка.
    std::size_t m_fallback = 0;
    float m_pixel_height = 0.0f;
    float m_ascent = 0.0f;
    float m_descent = 0.0f;
    float m_line_height = 0.0f;
    std::string m_source;
};

} // namespace RendererSystem
