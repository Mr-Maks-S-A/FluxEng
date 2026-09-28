#include <RendererSystem/Batch/SpriteBatch.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace RendererSystem {

void SpriteBatch::reserve(std::size_t sprites) {
    m_sprites.reserve(sprites);
    m_order.reserve(sprites);
    m_vertices.reserve(sprites * 4);
}

void SpriteBatch::clear() noexcept {
    m_sprites.clear();
    m_order.clear();
    m_vertices.clear();
    m_commands.clear();
}

void SpriteBatch::submit_rect(const Rect& rect, Color color, std::int32_t layer) {
    submit(SpriteInstance{
        .position = rect.position,
        .size = rect.size,
        .pivot = {0.0f, 0.0f},
        .rotation = 0.0f,
        .uv = {},
        .color = color,
        .texture = TextureHandle::white(),
        .layer = layer,
        .flip = SpriteFlip::None,
    });
}

void SpriteBatch::submit_line(glm::vec2 from, glm::vec2 to, float thickness, Color color, std::int32_t layer) {
    const glm::vec2 delta = to - from;
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (length <= 0.0f || thickness <= 0.0f) {
        return;
    }
    submit(SpriteInstance{
        .position = (from + to) * 0.5f,
        .size = {length, thickness},
        .pivot = {0.5f, 0.5f},
        .rotation = std::atan2(delta.y, delta.x),
        .uv = {},
        .color = color,
        .texture = TextureHandle::white(),
        .layer = layer,
        .flip = SpriteFlip::None,
    });
}

void SpriteBatch::submit_rect_outline(const Rect& rect, float thickness, Color color, std::int32_t layer) {
    const float t = std::min({thickness, rect.size.x * 0.5f, rect.size.y * 0.5f});
    if (t <= 0.0f) {
        return;
    }
    const glm::vec2 p = rect.position;
    const glm::vec2 s = rect.size;
    submit_rect({p, {s.x, t}}, color, layer);                              // верх
    submit_rect({{p.x, p.y + s.y - t}, {s.x, t}}, color, layer);           // низ
    submit_rect({{p.x, p.y + t}, {t, s.y - 2 * t}}, color, layer);         // лево
    submit_rect({{p.x + s.x - t, p.y + t}, {t, s.y - 2 * t}}, color, layer); // право
}

std::uint64_t SpriteBatch::sort_key(const SpriteInstance& sprite) const noexcept {
    // Сдвиг знакового слоя в беззнаковый диапазон сохраняет порядок: INT32_MIN → 0.
    const std::uint64_t layer = static_cast<std::uint32_t>(sprite.layer) ^ 0x8000'0000u;
    const std::uint64_t texture = m_mode == SortMode::LayerThenTexture ? sprite.texture.index : 0u;
    return (layer << 32) | texture;
}

void SpriteBatch::build() {
    const std::size_t count = m_sprites.size();

    m_order.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        m_order[i] = {sort_key(m_sprites[i]), static_cast<std::uint32_t>(i)};
    }
    if (!std::ranges::is_sorted(m_order)) {
        sort_order();
    }

    m_vertices.resize(count * 4);
    m_commands.clear();
    for (std::size_t quad = 0; quad < count; ++quad) {
        const SpriteInstance& sprite = m_sprites[m_order[quad].second];
        write_quad(sprite, &m_vertices[quad * 4]);

        if (m_commands.empty() || m_commands.back().texture != sprite.texture) {
            m_commands.push_back(DrawCommand{sprite.texture, static_cast<std::uint32_t>(quad), 1});
        } else {
            ++m_commands.back().quad_count;
        }
    }
}

void SpriteBatch::sort_order() {
    const std::size_t count = m_order.size();

    // На маленьких объёмах накладные расходы гистограмм не окупаются.
    // Пары (ключ, индекс) уникальны, поэтому результат совпадает со стабильной сортировкой.
    if (count < 256) {
        std::ranges::sort(m_order);
        return;
    }

    // LSD radix sort по байтам 64-битного ключа. Сортировка стабильна, а исходный порядок —
    // по индексу отправки, поэтому равные ключи остаются в порядке submit().
    std::array<std::array<std::uint32_t, 256>, 8> counts{};
    for (const auto& [key, index] : m_order) {
        for (std::size_t byte = 0; byte < 8; ++byte) {
            ++counts[byte][(key >> (byte * 8)) & 0xFFu];
        }
    }

    m_scratch.resize(count);
    auto* source = &m_order;
    auto* target = &m_scratch;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        auto& histogram = counts[byte];
        // Разряд, в котором у всех ключей один и тот же байт (например одинаковый слой), пропускаем.
        if (histogram[(source->front().first >> (byte * 8)) & 0xFFu] == count) {
            continue;
        }
        std::uint32_t offset = 0;
        for (std::uint32_t& bucket : histogram) {
            const std::uint32_t size = bucket;
            bucket = offset;
            offset += size;
        }
        for (const auto& entry : *source) {
            (*target)[histogram[(entry.first >> (byte * 8)) & 0xFFu]++] = entry;
        }
        std::swap(source, target);
    }
    if (source != &m_order) {
        m_order.swap(m_scratch);
    }
}

void SpriteBatch::write_quad(const SpriteInstance& sprite, SpriteVertex* out) noexcept {
    const glm::vec2 origin = sprite.size * sprite.pivot;
    glm::vec2 corners[4] = {
        glm::vec2{0.0f, 0.0f} - origin,
        glm::vec2{sprite.size.x, 0.0f} - origin,
        sprite.size - origin,
        glm::vec2{0.0f, sprite.size.y} - origin,
    };

    if (sprite.rotation != 0.0f) {
        const float c = std::cos(sprite.rotation);
        const float s = std::sin(sprite.rotation);
        for (glm::vec2& corner : corners) {
            corner = {corner.x * c - corner.y * s, corner.x * s + corner.y * c};
        }
    }

    float u0 = sprite.uv.min.x;
    float u1 = sprite.uv.max.x;
    float v0 = sprite.uv.min.y;
    float v1 = sprite.uv.max.y;
    if (has_flag(sprite.flip, SpriteFlip::X)) {
        std::swap(u0, u1);
    }
    if (has_flag(sprite.flip, SpriteFlip::Y)) {
        std::swap(v0, v1);
    }

    out[0] = SpriteVertex{sprite.position + corners[0], {u0, v0}, sprite.color};
    out[1] = SpriteVertex{sprite.position + corners[1], {u1, v0}, sprite.color};
    out[2] = SpriteVertex{sprite.position + corners[2], {u1, v1}, sprite.color};
    out[3] = SpriteVertex{sprite.position + corners[3], {u0, v1}, sprite.color};
}

} // namespace RendererSystem
