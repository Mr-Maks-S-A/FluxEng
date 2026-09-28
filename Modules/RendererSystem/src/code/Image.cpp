#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Core/Image.hpp>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

// stb_image собирается здесь и только здесь; его собственные предупреждения нам не интересны.
#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#include <stb_image.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace RendererSystem {

Image::Image(int width, int height, Color fill) : m_width(width), m_height(height) {
    if (width < 0 || height < 0) {
        throw RendererError(std::format("Image: invalid size {}x{}", width, height));
    }
    m_pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill);
}

std::expected<Image, std::string> Image::decode(std::span<const std::byte> encoded) {
    if (encoded.empty() || encoded.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected(std::string("image data is empty or too large"));
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),
                                                static_cast<int>(encoded.size()), &width, &height, &channels,
                                                STBI_rgb_alpha);
    if (data == nullptr) {
        return std::unexpected(std::format("cannot decode image: {}", stbi_failure_reason()));
    }

    Image image(width, height);
    std::memcpy(image.m_pixels.data(), data, image.m_pixels.size() * sizeof(Color));
    stbi_image_free(data);
    return image;
}

std::expected<Image, std::string> Image::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("cannot open '{}'", path.string()));
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto image = decode(std::as_bytes(std::span(bytes)));
    if (!image) {
        return std::unexpected(std::format("'{}': {}", path.string(), image.error()));
    }
    return image;
}

Image Image::checkerboard(int width, int height, int cell, Color first, Color second) {
    if (cell <= 0) {
        throw RendererError("Image::checkerboard: cell size must be positive");
    }
    Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.set_pixel(x, y, ((x / cell + y / cell) % 2 == 0) ? first : second);
        }
    }
    return image;
}

Color Image::pixel(int x, int y) const noexcept {
    assert(x >= 0 && y >= 0 && x < m_width && y < m_height);
    return m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x)];
}

void Image::set_pixel(int x, int y, Color color) noexcept {
    assert(x >= 0 && y >= 0 && x < m_width && y < m_height);
    m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x)] = color;
}

void Image::fill_rect(int x, int y, int width, int height, Color color) noexcept {
    const int x0 = std::max(x, 0);
    const int y0 = std::max(y, 0);
    const int x1 = std::min(x + width, m_width);
    const int y1 = std::min(y + height, m_height);
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            set_pixel(col, row, color);
        }
    }
}

void Image::flip_vertically() noexcept {
    const auto row = static_cast<std::size_t>(m_width);
    for (int top = 0, bottom = m_height - 1; top < bottom; ++top, --bottom) {
        std::swap_ranges(m_pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(top) * row),
                         m_pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(top + 1) * row),
                         m_pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(bottom) * row));
    }
}

} // namespace RendererSystem
