// Реализация stb_image_write нужна тестам, чтобы получать PNG из пикселей; AssetSystem её не содержит.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
