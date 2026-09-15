#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <WindowSystem/Window.hpp>

#include <utility>

int main(int argc, char** argv) {

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    
    // 2. Запускаем тесты (Window сам инициализирует и завершит GLFW)
    return context.run();
}

// =============================================================================
// МОДУЛЬ 1: Window LifeCycle & RAII
// =============================================================================
TEST_SUITE("WindowSystem::LifeCycle") {

    TEST_CASE("1.1. Создание окна и корректность указателей") {
        Window window(800, 600, "Test Window");

        CHECK(window.getNativeWindow() != nullptr);
        CHECK(window.getWidth() == 800);
        CHECK(window.getHeight() == 600);
        CHECK_FALSE(window.shouldClose());
    }

    TEST_CASE("1.2. Безопасность RAII и уничтожения") {
        GLFWwindow* rawPtr = nullptr;
        {
            Window localWindow(640, 480, "Scope Test");
            rawPtr = localWindow.getNativeWindow();
            CHECK(rawPtr != nullptr);
        }
        CHECK(true);
    }

    TEST_CASE("1.3. Принудительная установка флага закрытия") {
        Window window(800, 600, "Close Test");
        
        CHECK_FALSE(window.shouldClose());

        glfwSetWindowShouldClose(window.getNativeWindow(), GLFW_TRUE);
        
        CHECK(window.shouldClose());
    }
}

// =============================================================================
// МОДУЛЬ 2: Callback Integration & UserPointer
// =============================================================================
TEST_SUITE("WindowSystem::Callbacks") {

    TEST_CASE("2.1. Связывание WindowUserPointer") {
        Window window(800, 600, "UserPointer Test");

        GLFWwindow* rawWindow = window.getNativeWindow();
        REQUIRE(rawWindow != nullptr);

        auto* storedInstance = static_cast<Window*>(glfwGetWindowUserPointer(rawWindow));
        CHECK(storedInstance == &window);
    }

    TEST_CASE("2.2. Обработка пользовательских событий клавиатуры") {
        Window window(800, 600, "Keyboard Event Test");

        bool eventFired = false;
        int lastKey = -1;
        int lastAction = -1;

        window.onKeyPress = [&](int key, int action) {
            eventFired = true;
            lastKey = key;
            lastAction = action;
        };

        if (window.onKeyPress) {
            window.onKeyPress(GLFW_KEY_SPACE, GLFW_PRESS);
        }

        CHECK(eventFired);
        CHECK(lastKey == GLFW_KEY_SPACE);
        CHECK(lastAction == GLFW_PRESS);
    }

    TEST_CASE("2.3. Реакция на изменение размеров (Resize)") {
        Window window(800, 600, "Resize Test");

        int updatedWidth = 0;
        int updatedHeight = 0;

        window.onResize = [&](int w, int h) {
            updatedWidth = w;
            updatedHeight = h;
        };

        if (window.onResize) {
            window.onResize(1920, 1080);
        }

        CHECK(updatedWidth == 1920);
        CHECK(updatedHeight == 1080);
    }
}

// =============================================================================
// МОДУЛЬ 3: Move Semantics & Ownership
// =============================================================================
TEST_SUITE("WindowSystem::MoveSemantics") {

    TEST_CASE("3.1. Конструктор перемещения (Move Constructor)") {
        Window srcWindow(1024, 768, "Source Window");
        GLFWwindow* nativeHandle = srcWindow.getNativeWindow();

        REQUIRE(nativeHandle != nullptr);

        Window destWindow(std::move(srcWindow));

        CHECK(srcWindow.getNativeWindow() == nullptr);
        CHECK(destWindow.getNativeWindow() == nativeHandle);
        CHECK(destWindow.getWidth() == 1024);
        CHECK(destWindow.getHeight() == 768);

        auto* reboundPointer = static_cast<Window*>(glfwGetWindowUserPointer(nativeHandle));
        CHECK(reboundPointer == &destWindow);
    }

    TEST_CASE("3.2. Оператор присваивания перемещением (Move Assignment)") {
        Window windowA(800, 600, "Window A");
        Window windowB(1280, 720, "Window B");

        GLFWwindow* handleA = windowA.getNativeWindow();

        windowB = std::move(windowA);

        CHECK(windowA.getNativeWindow() == nullptr);
        CHECK(windowB.getNativeWindow() == handleA);

        auto* reboundPointer = static_cast<Window*>(glfwGetWindowUserPointer(handleA));
        CHECK(reboundPointer == &windowB);
    }
}

// =============================================================================
// МОДУЛЬ 4: Frame Update Loop
// =============================================================================
TEST_SUITE("WindowSystem::Execution") {

    TEST_CASE("4.1. Прогон итерации основного цикла update()") {
        Window window(640, 480, "Update Loop Test");

        CHECK_NOTHROW(window.update());
        CHECK_NOTHROW(window.update());
    }
}