#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <WindowSystem/Window.hpp>
#include <RendererSystem/OpenGLRenderer.hpp>
#include <RendererSystem/Texture.hpp>
#include <RendererSystem/Shader.hpp>

TEST_SUITE("RendererSystem") {

    TEST_CASE("OpenGLRenderer initialization and lifecycle") {
        // Создаем скрытое окно (visible = false) для headless тестов
        Window window(800, 600, "Test Window", false);
        Renderer::OpenGLRenderer renderer;

        SUBCASE("Successful initialization with valid window") {
            CHECK(renderer.initialize(window) == true);
            CHECK(renderer.getDeltaTime() >= 0.0f);
        }

        SUBCASE("Settings and configuration") {
            renderer.initialize(window);
            
            // Проверка изменения лимитов батчинга
            renderer.setMaxSpritesPerBatch(500);
            renderer.setBatchingEnabled(false);
            renderer.setBatchingEnabled(true);
            
            CHECK(true); // Прошла настройка без падающих ассертов
        }

        SUBCASE("Render frame cycle execution") {
            REQUIRE(renderer.initialize(window) == true);

            // Проверяем выполнения одного тестового кадра
            renderer.beginFrame();
            renderer.fillRect(10.0f, 10.0f, 100.0f, 100.0f, 0xFF0000FF);
            renderer.drawLine(0.0f, 0.0f, 50.0f, 50.0f, 0x00FF00FF);
            renderer.drawRect(20.0f, 20.0f, 30.0f, 30.0f, 0x0000FFFF);
            renderer.endFrame();

            CHECK(renderer.getDeltaTime() >= 0.0f);
        }

        SUBCASE("Explicit shutdown") {
            renderer.initialize(window);
            renderer.shutdown();
            CHECK(true);
        }
    }

    TEST_CASE("Shader source compilation test") {
        Window window(800, 600, "Shader Test Window", false);

        std::string validVertexShader = R"(
            #version 330 core
            layout (location = 0) in vec2 aPos;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        )";

        std::string validFragmentShader = R"(
            #version 330 core
            out vec4 FragColor;
            void main() {
                FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            }
        )";

        Renderer::Shader shader;
        bool compiled = shader.loadFromSource(validVertexShader, validFragmentShader);
        
        CHECK(compiled == true);
        CHECK(shader.getProgramID() != 0);
    }

    TEST_CASE("Texture creation in OpenGL Context") {
        Window window(800, 600, "Texture Test Window", false);

        Renderer::Texture texture;
        CHECK_FALSE(texture.isValid());

        // Создаем пустую RGBA текстуру 2x2 в памяти
        uint8_t dummyPixels[16] = { 255 }; 
        texture.create(2, 2, dummyPixels);

        CHECK(texture.isValid());
        CHECK(texture.getWidth() == 2);
        CHECK(texture.getHeight() == 2);
    }
}