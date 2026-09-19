// #include <iostream>
// #include <chrono>
// #include <vector>
// #include <random>
// #include <glad/glad.h>
// #include <WindowSystem/Window.hpp>
// #include <RendererSystem/Shader.hpp>

// // --- МОДУЛЬ 1: ИМПОРТ ECS ---
// #include <ECSSystem/World.h>

// // --- МОДУЛЬ 2: ИМПОРТ СИСТЕМЫ СОБЫТИЙ ---
// #include <EventSystem/Bus/EventBus.hpp>
// #include <EventSystem/Core/DeterministicTypeRegistry.hpp>
// #include <EventSystem/Storage/SoAEventBucket.hpp>

// // 1. Компоненты ECS
// struct Transform {
//     float x{0.0f}, y{0.0f}, z{0.0f};
// };

// struct RigidBody {
//     float vx{0.0f}, vy{0.0f};
// };

// struct Health {
//     float hp{100.0f};
// };

// // 2. События EventBus
// using ApplyImpulseBucket = EventSystem::Storage::SoAEventBucket<FluxECS::Entity, float, float>;
// using DamageEventBucket   = EventSystem::Storage::SoAEventBucket<FluxECS::Entity, float>;

// REGISTER_EVENT_TYPE(ApplyImpulseBucket)
// REGISTER_EVENT_TYPE(DamageEventBucket)

// enum class EngineMode {
//     Cap60FPS,
//     MaxUncappedFPS
// };

// constexpr EngineMode CURRENT_MODE = EngineMode::MaxUncappedFPS;
// constexpr size_t ENTITY_COUNT = 10000; // Количество объектов для стресс-теста

// int main() {
//     Window window(800, 600, "FluxEng - Stress Test (10k Entities + Damage)", true);

//     #ifdef GLFW_VERSION_MAJOR
//         glfwSwapInterval(0);
//     #endif

//     FluxECS::World ecsWorld;
//     EventSystem::Bus::EventBus eventBus;

//     // Резервируем память под события
//     eventBus.register_event<ApplyImpulseBucket>(ENTITY_COUNT);
//     eventBus.register_event<DamageEventBucket>(1000);

//     // Генерация случайных значений
//     std::mt19937 rng(1337);
//     std::uniform_real_distribution<float> distPos(-0.9f, 0.9f);
//     std::uniform_real_distribution<float> distVel(-0.5f, 0.5f);
//     std::uniform_real_distribution<float> distImpulse(-0.1f, 0.1f);
//     std::uniform_real_distribution<float> distDamage(5.0f, 25.0f);

//     std::vector<FluxECS::Entity> entities;
//     entities.reserve(ENTITY_COUNT);

//     // Спавн 10 000 сущностей
//     for (size_t i = 0; i < ENTITY_COUNT; ++i) {
//         FluxECS::Entity e = ecsWorld.create_entity();
//         ecsWorld.add_component<Transform>(e, distPos(rng), distPos(rng), 0.0f);
//         ecsWorld.add_component<RigidBody>(e, distVel(rng), distVel(rng));
//         ecsWorld.add_component<Health>(e, 100.0f);
//         entities.push_back(e);
//     }

//     // Очень маленький треугольник для рендеринга тысяч объектов
//     float vertices[] = {
//         -0.005f, -0.005f, 0.0f,  1.0f, 0.2f, 0.2f,
//          0.005f, -0.005f, 0.0f,  0.2f, 1.0f, 0.2f,
//          0.000f,  0.005f, 0.0f,  0.2f, 0.2f, 1.0f
//     };

//     unsigned int VAO, VBO;
//     glGenVertexArrays(1, &VAO);
//     glGenBuffers(1, &VBO);

//     glBindVertexArray(VAO);
//     glBindBuffer(GL_ARRAY_BUFFER, VBO);
//     glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

//     glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
//     glEnableVertexAttribArray(0);
//     glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
//     glEnableVertexAttribArray(1);

//     std::string vertexShaderSource = R"(
//         #version 330 core
//         layout (location = 0) in vec3 aPos;
//         layout (location = 1) in vec3 aColor;

//         out vec3 ourColor;
//         uniform vec2 uOffset;

//         void main() {
//             gl_Position = vec4(aPos.x + uOffset.x, aPos.y + uOffset.y, aPos.z, 1.0);
//             ourColor = aColor;
//         }
//     )";

//     std::string fragmentShaderSource = R"(
//         #version 330 core
//         out vec4 FragColor;
//         in vec3 ourColor;

//         void main() { FragColor = vec4(ourColor, 1.0); }
//     )";

//     Renderer::Shader shader;
//     if (!shader.loadFromSource(vertexShaderSource, fragmentShaderSource)) {
//         return -1;
//     }

//     auto lastTime = std::chrono::high_resolution_clock::now();
//     double fpsTimer = 0.0;
//     uint64_t frameCount = 0;

//     while (!window.shouldClose()) {
//         auto currentTime = std::chrono::high_resolution_clock::now();
//         float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
//         lastTime = currentTime;

//         fpsTimer += deltaTime;
//         frameCount++;
//         if (fpsTimer >= 1.0) {
//             float avgFrameTimeMs = (1000.0f * static_cast<float>(fpsTimer)) / static_cast<float>(frameCount);
//             std::cout << "[STRESS TEST] FPS: " << frameCount 
//                       << " | Active Entities: " << ecsWorld.get_entity_count()
//                       << " | Avg Frame: " << avgFrameTimeMs << " ms\n";
//             frameCount = 0;
//             fpsTimer = 0.0;
//         }

//         // ----------------------------------------------------------------------
//         // 1. ЭМИССИЯ СОБЫТИЙ (Безопасная для пустого списка)
//         // ----------------------------------------------------------------------
//         if (!entities.empty()) {
//             std::uniform_int_distribution<size_t> distEntityIdx(0, entities.size() - 1);

//             // Редкие импульсы
//             for (size_t k = 0; k < 20; ++k) {
//                 FluxECS::Entity target = entities[distEntityIdx(rng)];
//                 eventBus.emit<ApplyImpulseBucket>(target, distImpulse(rng), distImpulse(rng));
//             }

//             // Дозируем урон с учетом deltaTime, чтобы они не умирали мгновенно
//             for (size_t k = 0; k < 5; ++k) {
//                 FluxECS::Entity target = entities[distEntityIdx(rng)];
//                 eventBus.emit<DamageEventBucket>(target, distDamage(rng) * deltaTime * 60.0f);
//             }
//         }

//         // ... (блок обработки импульсов остается без изменений) ...

//         // ----------------------------------------------------------------------
//         // 3. ОБРАБОТКА СОБЫТИЙ УРОНА (Без дубликатов)
//         // ----------------------------------------------------------------------
//         std::vector<FluxECS::Entity> toDestroy;
//         auto* damageBucket = eventBus.get_bucket<DamageEventBucket>();
        
//         if (damageBucket) {
//             auto eArr   = damageBucket->get_stream<0>();
//             auto dmgArr = damageBucket->get_stream<1>();

//             for (size_t i = 0; i < damageBucket->size(); ++i) {
//                 FluxECS::Entity target = eArr[i];
//                 if (ecsWorld.has_component<Health>(target)) {
//                     auto* hp = ecsWorld.get_component<Health>(target);
//                     hp->hp -= dmgArr[i];

//                     // Добавляем на удаление только если объект еще жив в этом кадре
//                     if (hp->hp <= 0.0f) {
//                         toDestroy.push_back(target);
//                         // Зануляем HP, чтобы избежать повторного добавления за один кадр
//                         hp->hp = 999999.0f; 
//                     }
//                 }
//             }
//         }

//         // Безопасное удаление из ECS и из вектора хранения
//         for (FluxECS::Entity deadEntity : toDestroy) {
//             if (ecsWorld.has_component<Transform>(deadEntity)) {
//                 ecsWorld.destroy_entity(deadEntity);

//                 // Удаляем сущность из локального вектора спавна
//                 auto it = std::find(entities.begin(), entities.end(), deadEntity);
//                 if (it != entities.end()) {
//                     *it = entities.back();
//                     entities.pop_back();
//                 }
//             }
//         }

//         // ----------------------------------------------------------------------
//         // 4. ECS ФИЗИКА (Перемещение + Отскок от границ)
//         // ----------------------------------------------------------------------
//         ecsWorld.for_each<Transform, RigidBody>([deltaTime](FluxECS::Entity e, Transform& t, RigidBody& rb) {
//             t.x += rb.vx * deltaTime;
//             t.y += rb.vy * deltaTime;

//             // Отскок от границ экрана [-0.95, 0.95]
//             if (t.x < -0.95f || t.x > 0.95f) rb.vx *= -1.0f;
//             if (t.y < -0.95f || t.y > 0.95f) rb.vy *= -1.0f;
//         });

//         // ----------------------------------------------------------------------
//         // 5. РЕНДЕРИНГ
//         // ----------------------------------------------------------------------
//         glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
//         glClear(GL_COLOR_BUFFER_BIT);

//         shader.use();
//         glBindVertexArray(VAO);

//         // Инстансинг без оптимизаций OpenGL (Draw call на каждый объект для нагрузочного теста)
//         ecsWorld.for_each<Transform>([&](FluxECS::Entity e, const Transform& t) {
//             shader.setVec2("uOffset", t.x, t.y);
//             glDrawArrays(GL_TRIANGLES, 0, 3);
//         });

//         window.update();
//         eventBus.clear_all();

//         if constexpr (CURRENT_MODE == EngineMode::Cap60FPS) {
//             constexpr double targetFrameTime = 1.0 / 60.0;
//             auto frameEndTime = std::chrono::high_resolution_clock::now();
//             float elapsed = std::chrono::duration<float, std::chrono::seconds::period>(frameEndTime - currentTime).count();

//             if (elapsed < targetFrameTime) {
//                 std::this_thread::sleep_for(std::chrono::duration<double>(targetFrameTime - elapsed));
//             }
//         }
//     }

//     glDeleteVertexArrays(1, &VAO);
//     glDeleteBuffers(1, &VBO);

//     return 0;
// }


















// #include <iostream>
// #include <chrono>
// #include <vector>
// #include <random>
// #include <algorithm>
// #include <thread>
// #include <cmath>
// #include <glad/glad.h>
// #include <WindowSystem/Window.hpp>
// #include <RendererSystem/Shader.hpp>

// // --- МОДУЛЬ 1: ИМПОРТ ECS ---
// #include <ECSSystem/World.h>

// // НАСТРОЙКИ СИМУЛЯЦИИ
// constexpr int GRID_WIDTH = 300;
// constexpr int GRID_HEIGHT = 300;
// constexpr size_t SAND_COUNT = 10'000;            // 10 000 сущностей
// constexpr float FLIP_INTERVAL_SEC = 20.0f;       // Переворот каждые 10 секунд

// struct SandParticle {
//     int gx{0}, gy{0};
// };

// struct Transform {
//     float x{0.0f}, y{0.0f};
// };

// enum class EngineMode {
//     Cap60FPS,
//     MaxUncappedFPS
// };

// constexpr EngineMode CURRENT_MODE = EngineMode::MaxUncappedFPS;

// struct SandGrid {
//     std::vector<bool> cells;
//     std::vector<bool> walls;

//     SandGrid() : cells(GRID_WIDTH * GRID_HEIGHT, false), walls(GRID_WIDTH * GRID_HEIGHT, false) {
//         for (int y = 0; y < GRID_HEIGHT; ++y) {
//             for (int x = 0; x < GRID_WIDTH; ++x) {
//                 if (x == 0 || x == GRID_WIDTH - 1 || y == 0 || y == GRID_HEIGHT - 1) {
//                     setWall(x, y, true);
//                     continue;
//                 }

//                 int midX = GRID_WIDTH / 2;
//                 int midY = GRID_HEIGHT / 2;
//                 int distY = std::abs(y - midY);
//                 int allowedX = distY + 4; // Расширенное горлышко для пропуска 10k частиц

//                 if (std::abs(x - midX) > allowedX) {
//                     setWall(x, y, true);
//                 }
//             }
//         }
//     }

//     bool isBlocked(int x, int y) const {
//         if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return true;
//         size_t idx = static_cast<size_t>(y * GRID_WIDTH + x);
//         return walls[idx] || cells[idx];
//     }

//     void setCell(int x, int y, bool val) {
//         if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
//             cells[static_cast<size_t>(y * GRID_WIDTH + x)] = val;
//         }
//     }

//     void setWall(int x, int y, bool val) {
//         if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
//             walls[static_cast<size_t>(y * GRID_WIDTH + x)] = val;
//         }
//     }

//     void clearCells() {
//         std::fill(cells.begin(), cells.end(), false);
//     }
// };

// int main() {
//     Window window(800, 800, "FluxEng - 10k Instanced Sand Simulation", true);

//     #ifdef GLFW_VERSION_MAJOR
//         glfwSwapInterval(0); // Включаем Uncapped для точных замеров FPS
//     #endif

//     FluxECS::World ecsWorld;
//     SandGrid grid;

//     std::mt19937 rng(1337);
//     std::uniform_int_distribution<int> distStartX(GRID_WIDTH / 4, (3 * GRID_WIDTH) / 4);
//     std::uniform_int_distribution<int> distStartY(GRID_HEIGHT / 2 + 10, GRID_HEIGHT - 5);

//     // 1. Безопасный спавн 10 000 частиц
//     size_t spawned = 0;
//     int maxAttempts = 300000;

//     while (spawned < SAND_COUNT && maxAttempts-- > 0) {
//         int rx = distStartX(rng);
//         int ry = distStartY(rng);

//         if (!grid.isBlocked(rx, ry)) {
//             grid.setCell(rx, ry, true);
//             FluxECS::Entity e = ecsWorld.create_entity();

//             float worldX = (static_cast<float>(rx) / GRID_WIDTH) * 2.0f - 1.0f;
//             float worldY = (static_cast<float>(ry) / GRID_HEIGHT) * 2.0f - 1.0f;

//             ecsWorld.add_component<SandParticle>(e, rx, ry);
//             ecsWorld.add_component<Transform>(e, worldX, worldY);
//             spawned++;
//         }
//     }

//     // 2. Генерация стен
//     std::vector<float> wallVertices;
//     float cellSizeX = 2.0f / GRID_WIDTH;
//     float cellSizeY = 2.0f / GRID_HEIGHT;

//     for (int y = 0; y < GRID_HEIGHT; ++y) {
//         for (int x = 0; x < GRID_WIDTH; ++x) {
//             if (grid.walls[y * GRID_WIDTH + x]) {
//                 float wx = (static_cast<float>(x) / GRID_WIDTH) * 2.0f - 1.0f;
//                 float wy = (static_cast<float>(y) / GRID_HEIGHT) * 2.0f - 1.0f;

//                 wallVertices.insert(wallVertices.end(), {
//                     wx, wy, 0.0f, 0.35f, 0.35f, 0.38f,
//                     wx + cellSizeX, wy, 0.0f, 0.35f, 0.35f, 0.38f,
//                     wx + cellSizeX, wy + cellSizeY, 0.0f, 0.35f, 0.35f, 0.38f,
//                     wx, wy, 0.0f, 0.35f, 0.35f, 0.38f,
//                     wx + cellSizeX, wy + cellSizeY, 0.0f, 0.35f, 0.35f, 0.38f,
//                     wx, wy + cellSizeY, 0.0f, 0.35f, 0.35f, 0.38f
//                 });
//             }
//         }
//     }

//     // 3. Вертексы базовой песчинки (Quad)
//     float sandVertices[] = {
//         0.0f,       0.0f,       0.0f,  0.95f, 0.75f, 0.3f,
//         cellSizeX,  0.0f,       0.0f,  0.95f, 0.75f, 0.3f,
//         cellSizeX,  cellSizeY,  0.0f,  0.95f, 0.75f, 0.3f,
//         0.0f,       0.0f,       0.0f,  0.95f, 0.75f, 0.3f,
//         cellSizeX,  cellSizeY,  0.0f,  0.95f, 0.75f, 0.3f,
//         0.0f,       cellSizeY,  0.0f,  0.95f, 0.75f, 0.3f,
//     };

//     // VAO & VBO для Инстансинга Песка
//     unsigned int sandVAO, sandVBO, instanceVBO;
//     glGenVertexArrays(1, &sandVAO);
//     glGenBuffers(1, &sandVBO);
//     glGenBuffers(1, &instanceVBO);

//     glBindVertexArray(sandVAO);
//     glBindBuffer(GL_ARRAY_BUFFER, sandVBO);
//     glBufferData(GL_ARRAY_BUFFER, sizeof(sandVertices), sandVertices, GL_STATIC_DRAW);

//     // Атрибуты меша (0: pos, 1: color)
//     glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
//     glEnableVertexAttribArray(0);
//     glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
//     glEnableVertexAttribArray(1);

//     // Инстанс-буфер смещений (Атрибут 2: uOffset)
//     glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
//     glBufferData(GL_ARRAY_BUFFER, SAND_COUNT * sizeof(float) * 2, nullptr, GL_STREAM_DRAW);
//     glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
//     glEnableVertexAttribArray(2);
//     glVertexAttribDivisor(2, 1); // Шаг 1 раз на каждый ИНСТАНС

//     // VAO Стен
//     unsigned int wallVAO, wallVBO;
//     glGenVertexArrays(1, &wallVAO);
//     glGenBuffers(1, &wallVBO);
//     glBindVertexArray(wallVAO);
//     glBindBuffer(GL_ARRAY_BUFFER, wallVBO);
//     glBufferData(GL_ARRAY_BUFFER, wallVertices.size() * sizeof(float), wallVertices.data(), GL_STATIC_DRAW);
//     glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
//     glEnableVertexAttribArray(0);
//     glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
//     glEnableVertexAttribArray(1);

//     // Шейдер с поддержкой Instancing
//     std::string vertexShaderSource = R"(
//         #version 330 core
//         layout (location = 0) in vec3 aPos;
//         layout (location = 1) in vec3 aColor;
//         layout (location = 2) in vec2 aInstanceOffset;

//         out vec3 ourColor;

//         void main() {
//             gl_Position = vec4(aPos.x + aInstanceOffset.x, aPos.y + aInstanceOffset.y, aPos.z, 1.0);
//             ourColor = aColor;
//         }
//     )";

//     std::string fragmentShaderSource = R"(
//         #version 330 core
//         out vec4 FragColor;
//         in vec3 ourColor;

//         void main() { FragColor = vec4(ourColor, 1.0); }
//     )";

//     Renderer::Shader shader;
//     if (!shader.loadFromSource(vertexShaderSource, fragmentShaderSource)) {
//         return -1;
//     }

//     auto lastTime = std::chrono::high_resolution_clock::now();
//     float flipTimer = 0.0f;
//     uint64_t frameCounter = 0;
//     int gravityY = -1;

//     struct SandData {
//         FluxECS::Entity entity;
//         int gy;
//     };
//     std::vector<SandData> sandList;
//     sandList.reserve(SAND_COUNT);

//     std::vector<float> instanceOffsets;
//     instanceOffsets.reserve(SAND_COUNT * 2);

//     while (!window.shouldClose()) {
//         auto currentTime = std::chrono::high_resolution_clock::now();
//         float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
//         lastTime = currentTime;
//         frameCounter++;

//         // 1. Покадровый лог в консоль
//         float frameTimeMs = deltaTime * 1000.0f;
//         float currentFPS = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
//         std::cout << "[FRAME #" << frameCounter << "]"
//                   << " FPS: " << static_cast<int>(currentFPS)
//                   << " | Entities: " << ecsWorld.get_entity_count()
//                   << " | Frame Time: " << frameTimeMs << " ms"
//                   << " | Flip In: " << (FLIP_INTERVAL_SEC - flipTimer) << "s\n";

//         // 2. Настраиваемый таймер переворота
//         flipTimer += deltaTime;
//         if (flipTimer >= FLIP_INTERVAL_SEC) {
//             flipTimer = 0.0f;
//             gravityY *= -1;
//             std::cout << "\n=======================================================\n";
//             std::cout << "[HOURGLASS] FLIPPED! New gravity direction: " << (gravityY < 0 ? "DOWN" : "UP");
//             std::cout << "\n=======================================================\n\n";
//         }

//         // 3. Симуляция физики
//         grid.clearCells();
//         sandList.clear();

//         ecsWorld.for_each<SandParticle>([&](FluxECS::Entity e, SandParticle& sand) {
//             grid.setCell(sand.gx, sand.gy, true);
//             sandList.push_back({e, sand.gy});
//         });

//         std::sort(sandList.begin(), sandList.end(), [gravityY](const SandData& a, const SandData& b) {
//             return gravityY < 0 ? (a.gy < b.gy) : (a.gy > b.gy);
//         });

//         instanceOffsets.clear();

//         for (const auto& item : sandList) {
//             if (!ecsWorld.has_component<SandParticle>(item.entity) || !ecsWorld.has_component<Transform>(item.entity)) {
//                 continue;
//             }

//             auto* sand = ecsWorld.get_component<SandParticle>(item.entity);
//             auto* t = ecsWorld.get_component<Transform>(item.entity);

//             if (!sand || !t) continue;

//             grid.setCell(sand->gx, sand->gy, false);

//             int targetY = sand->gy + gravityY;
//             int leftX = sand->gx - 1;
//             int rightX = sand->gx + 1;

//             if (!grid.isBlocked(sand->gx, targetY)) {
//                 sand->gy = targetY;
//             } else {
//                 bool canLeft = !grid.isBlocked(leftX, targetY);
//                 bool canRight = !grid.isBlocked(rightX, targetY);

//                 if (canLeft && canRight) {
//                     sand->gx = (rng() % 2 == 0) ? leftX : rightX;
//                     sand->gy = targetY;
//                 } else if (canLeft) {
//                     sand->gx = leftX;
//                     sand->gy = targetY;
//                 } else if (canRight) {
//                     sand->gx = rightX;
//                     sand->gy = targetY;
//                 }
//             }

//             grid.setCell(sand->gx, sand->gy, true);

//             t->x = (static_cast<float>(sand->gx) / GRID_WIDTH) * 2.0f - 1.0f;
//             t->y = (static_cast<float>(sand->gy) / GRID_HEIGHT) * 2.0f - 1.0f;

//             // Собираем данные в VBO инстансинга
//             instanceOffsets.push_back(t->x);
//             instanceOffsets.push_back(t->y);
//         }

//         // 4. Отрисовка
//         glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
//         glClear(GL_COLOR_BUFFER_BIT);

//         shader.use();

//         // Отрисовка стен
//         glBindVertexArray(wallVAO);
//         glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(wallVertices.size() / 6));

//         // Быстрая инстанс-отрисовка 10 000 песчинок (1 Draw Call)
//         glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
//         glBufferSubData(GL_ARRAY_BUFFER, 0, instanceOffsets.size() * sizeof(float), instanceOffsets.data());

//         glBindVertexArray(sandVAO);
//         glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(instanceOffsets.size() / 2));

//         window.update();

//         if constexpr (CURRENT_MODE == EngineMode::Cap60FPS) {
//             constexpr double targetFrameTime = 1.0 / 60.0;
//             auto frameEndTime = std::chrono::high_resolution_clock::now();
//             float elapsed = std::chrono::duration<float, std::chrono::seconds::period>(frameEndTime - currentTime).count();

//             if (elapsed < targetFrameTime) {
//                 std::this_thread::sleep_for(std::chrono::duration<double>(targetFrameTime - elapsed));
//             }
//         }
//     }

//     glDeleteVertexArrays(1, &sandVAO);
//     glDeleteBuffers(1, &sandVBO);
//     glDeleteBuffers(1, &instanceVBO);
//     glDeleteVertexArrays(1, &wallVAO);
//     glDeleteBuffers(1, &wallVBO);

//     return 0;
// }
























#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include <thread>
#include <cmath>
#include <glad/glad.h>
#include <WindowSystem/Window.hpp>
#include <RendererSystem/Shader.hpp>

// --- МОДУЛЬ 1: ИМПОРТ ECS ---
#include <ECSSystem/World.h>

// --- МОДУЛЬ 2: ИМПОРТ СИСТЕМЫ СОБЫТИЙ ---
#include <EventSystem/Bus/EventBus.hpp>
#include <EventSystem/Core/DeterministicTypeRegistry.hpp>
#include <EventSystem/Storage/SoAEventBucket.hpp>

// НАСТРОЙКИ СИМУЛЯЦИИ
constexpr int GRID_WIDTH = 300;
constexpr int GRID_HEIGHT = 300;
constexpr size_t SAND_COUNT = 10'000;            // 10 000 сущностей
constexpr float FLIP_INTERVAL_SEC = 20.0f;       // Переворот каждые 20 секунд

struct SandParticle {
    int gx{0}, gy{0};
};

struct Transform {
    float x{0.0f}, y{0.0f};
};

// ----------------------------------------------------------------------
// СОБЫТИЯ
// ----------------------------------------------------------------------
using FlipGravityBucket        = EventSystem::Storage::SoAEventBucket<int>; // newGravityY
using UpdateSandPositionBucket = EventSystem::Storage::SoAEventBucket<FluxECS::Entity, int, int>; // Entity, targetGx, targetGy

REGISTER_EVENT_TYPE(FlipGravityBucket)
REGISTER_EVENT_TYPE(UpdateSandPositionBucket)

enum class EngineMode {
    Cap60FPS,
    MaxUncappedFPS
};

constexpr EngineMode CURRENT_MODE = EngineMode::MaxUncappedFPS;

struct SandGrid {
    std::vector<bool> cells;
    std::vector<bool> walls;

    SandGrid() : cells(GRID_WIDTH * GRID_HEIGHT, false), walls(GRID_WIDTH * GRID_HEIGHT, false) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            for (int x = 0; x < GRID_WIDTH; ++x) {
                if (x == 0 || x == GRID_WIDTH - 1 || y == 0 || y == GRID_HEIGHT - 1) {
                    setWall(x, y, true);
                    continue;
                }

                int midX = GRID_WIDTH / 2;
                int midY = GRID_HEIGHT / 2;
                int distY = std::abs(y - midY);
                int allowedX = distY + 4; 

                if (std::abs(x - midX) > allowedX) {
                    setWall(x, y, true);
                }
            }
        }
    }

    bool isBlocked(int x, int y) const {
        if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return true;
        size_t idx = static_cast<size_t>(y * GRID_WIDTH + x);
        return walls[idx] || cells[idx];
    }

    void setCell(int x, int y, bool val) {
        if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
            cells[static_cast<size_t>(y * GRID_WIDTH + x)] = val;
        }
    }

    void setWall(int x, int y, bool val) {
        if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
            walls[static_cast<size_t>(y * GRID_WIDTH + x)] = val;
        }
    }

    void clearCells() {
        std::fill(cells.begin(), cells.end(), false);
    }
};

int main() {
    Window window(800, 800, "FluxEng - Event-Driven 10k Sand Simulation", true);

    #ifdef GLFW_VERSION_MAJOR
        glfwSwapInterval(0);
    #endif

    FluxECS::World ecsWorld;
    EventSystem::Bus::EventBus eventBus;

    // Резервируем память под события
    eventBus.register_event<FlipGravityBucket>(1);
    eventBus.register_event<UpdateSandPositionBucket>(SAND_COUNT);

    SandGrid grid;

    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> distStartX(GRID_WIDTH / 4, (3 * GRID_WIDTH) / 4);
    std::uniform_int_distribution<int> distStartY(GRID_HEIGHT / 2 + 10, GRID_HEIGHT - 5);

    // 1. Спавн 10 000 частиц
    size_t spawned = 0;
    int maxAttempts = 300000;

    while (spawned < SAND_COUNT && maxAttempts-- > 0) {
        int rx = distStartX(rng);
        int ry = distStartY(rng);

        if (!grid.isBlocked(rx, ry)) {
            grid.setCell(rx, ry, true);
            FluxECS::Entity e = ecsWorld.create_entity();

            float worldX = (static_cast<float>(rx) / GRID_WIDTH) * 2.0f - 1.0f;
            float worldY = (static_cast<float>(ry) / GRID_HEIGHT) * 2.0f - 1.0f;

            ecsWorld.add_component<SandParticle>(e, rx, ry);
            ecsWorld.add_component<Transform>(e, worldX, worldY);
            spawned++;
        }
    }

    // 2. Генерация стен
    std::vector<float> wallVertices;
    float cellSizeX = 2.0f / GRID_WIDTH;
    float cellSizeY = 2.0f / GRID_HEIGHT;

    for (int y = 0; y < GRID_HEIGHT; ++y) {
        for (int x = 0; x < GRID_WIDTH; ++x) {
            if (grid.walls[y * GRID_WIDTH + x]) {
                float wx = (static_cast<float>(x) / GRID_WIDTH) * 2.0f - 1.0f;
                float wy = (static_cast<float>(y) / GRID_HEIGHT) * 2.0f - 1.0f;

                wallVertices.insert(wallVertices.end(), {
                    wx, wy, 0.0f, 0.35f, 0.35f, 0.38f,
                    wx + cellSizeX, wy, 0.0f, 0.35f, 0.35f, 0.38f,
                    wx + cellSizeX, wy + cellSizeY, 0.0f, 0.35f, 0.35f, 0.38f,
                    wx, wy, 0.0f, 0.35f, 0.35f, 0.38f,
                    wx + cellSizeX, wy + cellSizeY, 0.0f, 0.35f, 0.35f, 0.38f,
                    wx, wy + cellSizeY, 0.0f, 0.35f, 0.35f, 0.38f
                });
            }
        }
    }

    // 3. Вертексы базовой песчинки (Quad)
    float sandVertices[] = {
        0.0f,       0.0f,       0.0f,  0.95f, 0.75f, 0.3f,
        cellSizeX,  0.0f,       0.0f,  0.95f, 0.75f, 0.3f,
        cellSizeX,  cellSizeY,  0.0f,  0.95f, 0.75f, 0.3f,
        0.0f,       0.0f,       0.0f,  0.95f, 0.75f, 0.3f,
        cellSizeX,  cellSizeY,  0.0f,  0.95f, 0.75f, 0.3f,
        0.0f,       cellSizeY,  0.0f,  0.95f, 0.75f, 0.3f,
    };

    // VAO & VBO для Инстансинга Песка
    unsigned int sandVAO, sandVBO, instanceVBO;
    glGenVertexArrays(1, &sandVAO);
    glGenBuffers(1, &sandVBO);
    glGenBuffers(1, &instanceVBO);

    glBindVertexArray(sandVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sandVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sandVertices), sandVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, SAND_COUNT * sizeof(float) * 2, nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);

    // VAO Стен
    unsigned int wallVAO, wallVBO;
    glGenVertexArrays(1, &wallVAO);
    glGenBuffers(1, &wallVBO);
    glBindVertexArray(wallVAO);
    glBindBuffer(GL_ARRAY_BUFFER, wallVBO);
    glBufferData(GL_ARRAY_BUFFER, wallVertices.size() * sizeof(float), wallVertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Шейдеры
    std::string vertexShaderSource = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aColor;
        layout (location = 2) in vec2 aInstanceOffset;

        out vec3 ourColor;

        void main() {
            gl_Position = vec4(aPos.x + aInstanceOffset.x, aPos.y + aInstanceOffset.y, aPos.z, 1.0);
            ourColor = aColor;
        }
    )";

    std::string fragmentShaderSource = R"(
        #version 330 core
        out vec4 FragColor;
        in vec3 ourColor;

        void main() { FragColor = vec4(ourColor, 1.0); }
    )";

    Renderer::Shader shader;
    if (!shader.loadFromSource(vertexShaderSource, fragmentShaderSource)) {
        return -1;
    }

    auto lastTime = std::chrono::high_resolution_clock::now();
    float flipTimer = 0.0f;
    uint64_t frameCounter = 0;
    int gravityY = -1;

    struct SandData {
        FluxECS::Entity entity;
        int gy;
    };
    std::vector<SandData> sandList;
    sandList.reserve(SAND_COUNT);

    std::vector<float> instanceOffsets;
    instanceOffsets.reserve(SAND_COUNT * 2);

    while (!window.shouldClose()) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
        lastTime = currentTime;
        frameCounter++;

        float frameTimeMs = deltaTime * 1000.0f;
        float currentFPS = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
        std::cout << "[FRAME #" << frameCounter << "]"
                  << " FPS: " << static_cast<int>(currentFPS)
                  << " | Entities: " << ecsWorld.get_entity_count()
                  << " | Frame Time: " << frameTimeMs << " ms"
                  << " | Flip In: " << (FLIP_INTERVAL_SEC - flipTimer) << "s\n";

        // ----------------------------------------------------------------------
        // 1. ЭМИССИЯ СОБЫТИЙ СИСТЕМЫ (Таймеры, генерация намерений)
        // ----------------------------------------------------------------------
        flipTimer += deltaTime;
        if (flipTimer >= FLIP_INTERVAL_SEC) {
            flipTimer = 0.0f;
            eventBus.emit<FlipGravityBucket>(gravityY * -1);
        }

        // ----------------------------------------------------------------------
        // 2. ОБРАБОТКА СОБЫТИЙ: Переворот гравитации
        // ----------------------------------------------------------------------
        auto* flipBucket = eventBus.get_bucket<FlipGravityBucket>();
        if (flipBucket && flipBucket->size() > 0) {
            auto newGrav = flipBucket->get_stream<0>();
            gravityY = newGrav[0];

            std::cout << "\n=======================================================\n";
            std::cout << "[HOURGLASS] FLIPPED! New gravity direction: " << (gravityY < 0 ? "DOWN" : "UP");
            std::cout << "\n=======================================================\n\n";
        }

        // ----------------------------------------------------------------------
        // 3. РАСЧЁТ ФИЗИКИ И ЭМИССИЯ СОБЫТИЙ ПЕРЕМЕЩЕНИЯ
        // ----------------------------------------------------------------------
        grid.clearCells();
        sandList.clear();

        ecsWorld.for_each<SandParticle>([&](FluxECS::Entity e, SandParticle& sand) {
            grid.setCell(sand.gx, sand.gy, true);
            sandList.push_back({e, sand.gy});
        });

        std::sort(sandList.begin(), sandList.end(), [gravityY](const SandData& a, const SandData& b) {
            return gravityY < 0 ? (a.gy < b.gy) : (a.gy > b.gy);
        });

        for (const auto& item : sandList) {
            auto* sand = ecsWorld.get_component<SandParticle>(item.entity);
            if (!sand) continue;

            grid.setCell(sand->gx, sand->gy, false);

            int targetY = sand->gy + gravityY;
            int leftX = sand->gx - 1;
            int rightX = sand->gx + 1;
            int finalGx = sand->gx;
            int finalGy = sand->gy;

            if (!grid.isBlocked(sand->gx, targetY)) {
                finalGy = targetY;
            } else {
                bool canLeft = !grid.isBlocked(leftX, targetY);
                bool canRight = !grid.isBlocked(rightX, targetY);

                if (canLeft && canRight) {
                    finalGx = (rng() % 2 == 0) ? leftX : rightX;
                    finalGy = targetY;
                } else if (canLeft) {
                    finalGx = leftX;
                    finalGy = targetY;
                } else if (canRight) {
                    finalGx = rightX;
                    finalGy = targetY;
                }
            }

            grid.setCell(finalGx, finalGy, true);

            // Если позиция изменилась — генерируем событие перемещения
            if (finalGx != sand->gx || finalGy != sand->gy) {
                eventBus.emit<UpdateSandPositionBucket>(item.entity, finalGx, finalGy);
            }
        }

        // ----------------------------------------------------------------------
        // 4. ОБРАБОТКА СОБЫТИЙ: Мутация компонентов ECS и подготовка рендера
        // ----------------------------------------------------------------------
        auto* updateBucket = eventBus.get_bucket<UpdateSandPositionBucket>();
        if (updateBucket) {
            auto entityStream = updateBucket->get_stream<0>();
            auto gxStream     = updateBucket->get_stream<1>();
            auto gyStream     = updateBucket->get_stream<2>();

            for (size_t i = 0; i < updateBucket->size(); ++i) {
                FluxECS::Entity e = entityStream[i];
                if (ecsWorld.has_component<SandParticle>(e) && ecsWorld.has_component<Transform>(e)) {
                    auto* sand = ecsWorld.get_component<SandParticle>(e);
                    auto* t    = ecsWorld.get_component<Transform>(e);

                    sand->gx = gxStream[i];
                    sand->gy = gyStream[i];

                    t->x = (static_cast<float>(sand->gx) / GRID_WIDTH) * 2.0f - 1.0f;
                    t->y = (static_cast<float>(sand->gy) / GRID_HEIGHT) * 2.0f - 1.0f;
                }
            }
        }

        // Сборка VBO данных для отрисовки
        instanceOffsets.clear();
        ecsWorld.for_each<Transform>([&](FluxECS::Entity e, const Transform& t) {
            instanceOffsets.push_back(t.x);
            instanceOffsets.push_back(t.y);
        });

        // ----------------------------------------------------------------------
        // 5. РЕНДЕРИНГ
        // ----------------------------------------------------------------------
        glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        shader.use();

        // Отрисовка стен
        glBindVertexArray(wallVAO);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(wallVertices.size() / 6));

        // Инстансинг
        glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, instanceOffsets.size() * sizeof(float), instanceOffsets.data());

        glBindVertexArray(sandVAO);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(instanceOffsets.size() / 2));

        window.update();

        // Очистка событий в конце кадра
        eventBus.clear_all();

        if constexpr (CURRENT_MODE == EngineMode::Cap60FPS) {
            constexpr double targetFrameTime = 1.0 / 60.0;
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            float elapsed = std::chrono::duration<float, std::chrono::seconds::period>(frameEndTime - currentTime).count();

            if (elapsed < targetFrameTime) {
                std::this_thread::sleep_for(std::chrono::duration<double>(targetFrameTime - elapsed));
            }
        }
    }

    glDeleteVertexArrays(1, &sandVAO);
    glDeleteBuffers(1, &sandVBO);
    glDeleteBuffers(1, &instanceVBO);
    glDeleteVertexArrays(1, &wallVAO);
    glDeleteBuffers(1, &wallVBO);

    return 0;
}