#include <AssetSystem/Loaders.hpp>

#include <climits>
#include <string>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
// STB_IMAGE_STATIC: функции stb_image видны только этому файлу. RendererSystem подключает свою копию так же,
// поэтому оба модуля линкуются в одну программу без конфликта символов.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#include <stb_image.h>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace AssetSystem {

Result<ImageAsset> decode_image(std::span<const std::byte> encoded, const AssetLimits& limits) {
    if (encoded.empty()) return fail(ErrorCode::Decode, "empty image data");
    if (encoded.size() > static_cast<std::size_t>(INT_MAX)) return fail(ErrorCode::TooLarge, "image data is larger than 2 GiB");
    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int size = static_cast<int>(encoded.size());

    // Размеры — из заголовка, до выделения памяти под пиксели: враждебная картинка 60000×60000 не должна дойти до malloc.
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes, size, &width, &height, &channels)) {
        return fail(ErrorCode::Decode, std::string("image header: ") + stbi_failure_reason());
    }
    // Для BMP «сверху вниз» stbi_info возвращает отрицательную высоту (знак — порядок строк). INT_MIN не имеет модуля.
    if (height == INT_MIN) return fail(ErrorCode::Decode, "image has an invalid height");
    height = height < 0 ? -height : height;
    if (width <= 0 || height <= 0) return fail(ErrorCode::Decode, "image has no pixels");
    if (width > limits.max_image_side || height > limits.max_image_side ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > limits.max_image_pixels) {
        return fail(ErrorCode::TooLarge, "image " + std::to_string(width) + "x" + std::to_string(height) + " exceeds the limit");
    }

    stbi_uc* pixels = stbi_load_from_memory(bytes, size, &width, &height, &channels, 4);
    if (pixels == nullptr) return fail(ErrorCode::Decode, std::string("image data: ") + stbi_failure_reason());

    ImageAsset image;
    image.width = width;
    image.height = height;
    image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(pixels);
    return image;
}

} // namespace AssetSystem
