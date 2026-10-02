#include <RendererSystem/Core/Procedural.hpp>

#include <algorithm>
#include <cmath>

// stb_perlin собирается здесь и только здесь.
#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_PERLIN_IMPLEMENTATION
#include <stb_perlin.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace RendererSystem::Procedural {

float perlin(glm::vec3 point, std::uint8_t seed) noexcept {
    return stb_perlin_noise3_seed(point.x, point.y, point.z, 0, 0, 0, seed);
}

float fbm(glm::vec3 point, int octaves, float lacunarity, float gain) noexcept {
    return stb_perlin_fbm_noise3(point.x, point.y, point.z, lacunarity, gain, std::max(octaves, 1));
}

float ridge(glm::vec3 point, int octaves, float lacunarity, float gain, float offset) noexcept {
    return stb_perlin_ridge_noise3(point.x, point.y, point.z, lacunarity, gain, offset, std::max(octaves, 1));
}

float turbulence(glm::vec3 point, int octaves, float lacunarity, float gain) noexcept {
    return stb_perlin_turbulence_noise3(point.x, point.y, point.z, lacunarity, gain, std::max(octaves, 1));
}

Color sample_gradient(std::span<const GradientStop> stops, float t) noexcept {
    if (stops.empty()) {
        return Colors::black;
    }
    if (t <= stops.front().at) {
        return stops.front().color;
    }
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].at) {
            const float span = stops[i].at - stops[i - 1].at;
            const float k = span > 0.0f ? (t - stops[i - 1].at) / span : 1.0f;
            return Color::lerp(stops[i - 1].color, stops[i].color, k);
        }
    }
    return stops.back().color;
}

Image noise_image(const NoiseImageDesc& desc, std::span<const GradientStop> gradient) {
    Image image(desc.width, desc.height);
    const float step = desc.scale / static_cast<float>(std::max(desc.width, 1));
    for (int y = 0; y < desc.height; ++y) {
        for (int x = 0; x < desc.width; ++x) {
            const glm::vec3 p{static_cast<float>(x) * step, static_cast<float>(y) * step, desc.z};
            float value = 0.0f;
            switch (desc.kind) {
                case NoiseKind::Fbm: value = fbm(p, desc.octaves) * 0.5f + 0.5f; break;
                case NoiseKind::Ridge: value = ridge(p, desc.octaves) * 0.6f; break;
                case NoiseKind::Turbulence: value = turbulence(p, desc.octaves); break;
            }
            image.set_pixel(x, y, sample_gradient(gradient, std::clamp(value, 0.0f, 1.0f)));
        }
    }
    return image;
}

Image circle_image(int size, Color fill, float thickness) {
    Image image(size, size, Colors::transparent);
    const float radius = static_cast<float>(size) * 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - radius;
            const float dy = static_cast<float>(y) + 0.5f - radius;
            const float distance = std::sqrt(dx * dx + dy * dy);
            float coverage = std::clamp(radius - distance, 0.0f, 1.0f); // сглаженный внешний край
            if (thickness > 0.0f) {
                coverage *= std::clamp(distance - (radius - thickness), 0.0f, 1.0f); // и внутренний
            }
            if (coverage > 0.0f) {
                image.set_pixel(x, y, fill.with_alpha(static_cast<std::uint8_t>(static_cast<float>(fill.a) * coverage)));
            }
        }
    }
    return image;
}

} // namespace RendererSystem::Procedural
