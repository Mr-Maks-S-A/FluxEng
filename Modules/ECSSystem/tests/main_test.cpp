#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <ECSSystem/World.h>
#include <ECSSystem/Allocator.h>
#include <ECSSystem/SparseSet.h>

#include <iostream>
#include <vector>
#include <string_view>

// ==============================================================================
// Тестовые компоненты
// ==============================================================================
struct Transform {
    float x{0.0f}, y{0.0f}, z{0.0f};

    friend std::ostream& operator<<(std::ostream& os, const Transform& t) {
        return os << "Transform{" << t.x << ", " << t.y << ", " << t.z << "}";
    }
};

struct RigidBody {
    float vx{0.0f}, vy{0.0f};
    float mass{1.0f};

    friend std::ostream& operator<<(std::ostream& os, const RigidBody& rb) {
        return os << "RigidBody{v=(" << rb.vx << ", " << rb.vy << "), m=" << rb.mass << "}";
    }
};

struct NameTag {
    std::string_view name;

    friend std::ostream& operator<<(std::ostream& os, const NameTag& nt) {
        return os << "NameTag{\"" << nt.name << "\"}";
    }
};

// Хелпер для красивого вывода заголовков блоков в консоль
void printSectionHeader(std::string_view title) {
    std::cout << "\n==================================================\n"
              << "  [TEST STEP] " << title << "\n"
              << "==================================================\n";
}

// ==============================================================================
// Тестовые сюиты
// ==============================================================================

TEST_SUITE("FluxECS - Comprehensive Test Suite") {

    TEST_CASE("1. Entity Lifecycle and ID Recycling") {
        printSectionHeader("1. Entity Lifecycle and ID Recycling");
        
        FluxECS::World world;
        std::cout << "Initial entity count: " << world.get_entity_count() << "\n";
        CHECK(world.get_entity_count() == 0);

        // Создаём 5 сущностей
        std::vector<FluxECS::Entity> entities;
        for (int i = 0; i < 5; ++i) {
            auto e = world.create_entity();
            entities.push_back(e);
            std::cout << "-> Created Entity ID: " << e << "\n";
        }
        
        CHECK(world.get_entity_count() == 5);

        // Удаляем сущность из середины
        FluxECS::Entity deletedId = entities[2];
        std::cout << "-> Destroying Entity ID: " << deletedId << "\n";
        world.destroy_entity(deletedId);
        CHECK(world.get_entity_count() == 4);

        // Пересоздаём сущность: проверяем повторное использование ID (LIFO queue)
        FluxECS::Entity reusedId = world.create_entity();
        std::cout << "-> Created new Entity ID (expected recycling): " << reusedId << "\n";
        
        CHECK(reusedId == deletedId);
        CHECK(world.get_entity_count() == 5);
    }

    TEST_CASE("2. Component Storage Operations & Swap-Removal (SparseSet)") {
        printSectionHeader("2. Component Storage Operations & Swap-Removal");

        FluxECS::ComponentStorage<Transform> storage;

        // Вставляем элементы
        storage.emplace(10, 1.0f, 2.0f, 3.0f);
        storage.emplace(25, 4.0f, 5.0f, 6.0f);
        storage.emplace(3,  7.0f, 8.0f, 9.0f);

        std::cout << "Storage size after 3 emplacements: " << storage.size() << "\n";
        CHECK(storage.size() == 3);
        CHECK(storage.contains(10));
        CHECK(storage.contains(25));
        CHECK(storage.contains(3));
        CHECK_FALSE(storage.contains(99));

        std::cout << "Entity 10 Component: " << *storage.get(10) << "\n";
        std::cout << "Entity 25 Component: " << *storage.get(25) << "\n";

        // Проверяем удаление с перепаковкой (Swap-with-last)
        std::cout << "-> Removing component from Entity 10...\n";
        storage.remove(10);

        CHECK(storage.size() == 2);
        CHECK_FALSE(storage.contains(10));
        CHECK(storage.contains(25));
        CHECK(storage.contains(3));

        std::cout << "Remaining entities in dense array:\n";
        for (size_t i = 0; i < storage.size(); ++i) {
            FluxECS::Entity e = storage.entities()[i];
            std::cout << "  Index [" << i << "] -> Entity " << e 
                      << ": " << *storage.get(e) << "\n";
        }
    }

    TEST_CASE("3. Multi-Component Queries and Mask Matching (for_each)") {
        printSectionHeader("3. Multi-Component Queries and Mask Matching");

        FluxECS::World world;

        // E1: Transform + RigidBody + NameTag
        auto e1 = world.create_entity();
        world.add_component<Transform>(e1, 0.0f, 0.0f, 0.0f);
        world.add_component<RigidBody>(e1, 10.0f, 0.0f, 1.5f);
        world.add_component<NameTag>(e1, "Player");

        // E2: Transform (No RigidBody)
        auto e2 = world.create_entity();
        world.add_component<Transform>(e2, 100.0f, 50.0f, 0.0f);
        world.add_component<NameTag>(e2, "Static Obstacle");

        // E3: Transform + RigidBody
        auto e3 = world.create_entity();
        world.add_component<Transform>(e3, 5.0f, 5.0f, 0.0f);
        world.add_component<RigidBody>(e3, -2.0f, 3.0f, 0.5f);

        std::cout << "Entities created. Running Physics System query (Transform + RigidBody)...\n";

        size_t processedCount = 0;
        float dt = 0.5f;

        world.for_each<Transform, RigidBody>([&](FluxECS::Entity entity, Transform& t, const RigidBody& rb) {
            std::cout << "  [System Match] Entity " << entity << "\n"
                      << "    Before: " << t << ", " << rb << "\n";
            
            t.x += rb.vx * dt;
            t.y += rb.vy * dt;

            std::cout << "    After:  " << t << "\n";
            processedCount++;
        });

        CHECK(processedCount == 2); // Только E1 и E3
        CHECK(world.get_component<Transform>(e1)->x == 5.0f);
        CHECK(world.get_component<Transform>(e2)->x == 100.0f); // Не затронут
        CHECK(world.get_component<Transform>(e3)->x == 4.0f);
    }

    TEST_CASE("4. Component Cleanup on Entity Destruction") {
        printSectionHeader("4. Component Cleanup on Entity Destruction");

        FluxECS::World world;

        auto e = world.create_entity();
        world.add_component<Transform>(e, 1.0f, 2.0f, 3.0f);
        world.add_component<NameTag>(e, "TempObject");

        CHECK(world.has_component<Transform>(e));
        CHECK(world.has_component<NameTag>(e));

        std::cout << "Destroying Entity " << e << " with active components...\n";
        world.destroy_entity(e);

        CHECK_FALSE(world.has_component<Transform>(e));
        CHECK_FALSE(world.has_component<NameTag>(e));

        size_t count = 0;
        world.for_each<Transform>([&](FluxECS::Entity, Transform&) { count++; });
        CHECK(count == 0);
        std::cout << "Verified: No dangling components left in system.\n";
    }

    TEST_CASE("5. Custom PoolAllocator Capacity & Reuse") {
        printSectionHeader("5. Custom PoolAllocator Capacity & Reuse");

        constexpr size_t InitialCapacity = 2;
        FluxECS::PoolAllocator<Transform> pool(InitialCapacity);

        std::cout << "Allocating items beyond initial capacity (" << InitialCapacity << ")...\n";

        Transform* p1 = pool.allocate();
        Transform* p2 = pool.allocate();
        Transform* p3 = pool.allocate(); // Вызовет расширение пула

        CHECK(p1 != nullptr);
        CHECK(p2 != nullptr);
        CHECK(p3 != nullptr);

        p1->x = 100.0f;
        p3->x = 300.0f;

        std::cout << "p1->x: " << p1->x << ", p3->x: " << p3->x << "\n";
        CHECK(p1->x == 100.0f);
        CHECK(p3->x == 300.0f);

        std::cout << "Deallocating p2 and reallocating...\n";
        pool.deallocate(p2);

        Transform* p4 = pool.allocate();
        CHECK(p4 != nullptr);
        std::cout << "Allocation successful after deallocation.\n";

        pool.deallocate(p1);
        pool.deallocate(p3);
        pool.deallocate(p4);
    }
}

// Custom Runner с подробным логированием результатов
int main(int argc, char** argv) {
    doctest::Context context;

    // Настройка консольного вывода doctest
    context.setOption("no-breaks", true); // Не заходить в отладчик при ошибках
    context.setOption("success", true);   // Выводить успешные проверки (CHECK)
    context.applyCommandLine(argc, argv);

    std::cout << "\n==================================================\n";
    std::cout << "   STARTING FLUX-ECS UNIT TESTS & VALIDATION      \n";
    std::cout << "==================================================\n";

    int res = context.run();

    if (context.shouldExit()) {
        return res;
    }

    std::cout << "\n==================================================\n";
    std::cout << "   TEST RUN FINISHED WITH CODE: " << res << "\n";
    std::cout << "==================================================\n\n";

    return res;
}