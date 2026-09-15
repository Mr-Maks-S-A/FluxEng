#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <EventSystem/Bus/EventBus.hpp>
#include <EventSystem/Core/DeterministicTypeRegistry.hpp>
#include <EventSystem/Core/FNV1a.hpp>
#include <EventSystem/Storage/EventSchema.hpp>
#include <EventSystem/Storage/SoAEventBucket.hpp>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// =============================================================================
// ОБЪЯВЛЕНИЕ ТЕСТОВЫХ СОБЫТИЙ И БАКЕТОВ
// =============================================================================

struct DummyEventA {};
struct DummyEventB {};
struct ComplexEvent {
    int id;
    double timestamp;
};

using TestBucketSoA = EventSystem::Storage::SoAEventBucket<int, float, std::string>;
using TestBucketCollision = EventSystem::Storage::SoAEventBucket<uint32_t, uint32_t>;
using TestBucketMoveOnly = EventSystem::Storage::SoAEventBucket<std::unique_ptr<int>>;
using TestBucketStress = EventSystem::Storage::SoAEventBucket<uint64_t, double, int32_t>;

REGISTER_EVENT_TYPE(DummyEventA)
REGISTER_EVENT_TYPE(DummyEventB)
REGISTER_EVENT_TYPE(ComplexEvent)
REGISTER_EVENT_TYPE(TestBucketSoA)
REGISTER_EVENT_TYPE(TestBucketCollision)
REGISTER_EVENT_TYPE(TestBucketMoveOnly)
REGISTER_EVENT_TYPE(TestBucketStress)

// =============================================================================
// МОДУЛЬ 1: FNV1a (Тестирование хэширования)
// =============================================================================
TEST_SUITE("Core::FNV1a") {

    TEST_CASE("1.1. Вычисление в compile-time и детерминированность") {
        constexpr std::string_view str = "EventSystem_Test_String";
        constexpr auto hash1 = EventSystem::Core::FNV1a::hash(str);
        constexpr auto hash2 = EventSystem::Core::FNV1a::hash(str);

        CHECK(hash1 != 0);
        CHECK(hash1 == hash2);
    }

    TEST_CASE("1.2. Крайние случаи и дифференциация строк") {
        constexpr auto empty_hash = EventSystem::Core::FNV1a::hash("");
        constexpr auto hashA = EventSystem::Core::FNV1a::hash("EventA");
        constexpr auto hashB = EventSystem::Core::FNV1a::hash("EventB");
        constexpr auto hasha = EventSystem::Core::FNV1a::hash("eventA");

        CHECK(empty_hash == 14695981039346656037ULL);
        CHECK(hashA != empty_hash);
        CHECK(hashA != hashB);
        CHECK(hashA != hasha);
    }


    // -------------------------------------------------------------------------
    // Проверка кросс-компиляционного и стабильного детерминизма.
    // Golden values фиксируют контракт FNV-1a 64-bit.
    // При неизменном имени строки результат обязан быть одинаковым
    // независимо от компилятора, конфигурации сборки и платформы.
    // -------------------------------------------------------------------------
    TEST_CASE("1.3. Детерминизм FNV-1a относительно эталонных значений") {

        // Эталон FNV-1a 64-bit для пустой строки.
        constexpr uint64_t EXPECTED_EMPTY_HASH =
            0xcbf29ce484222325ULL;

        // Эталон FNV-1a 64-bit для строки "DummyEventA".
        constexpr uint64_t EXPECTED_DUMMY_EVENT_A =
            0x55ca0c6363781d98ULL;

        // Эталон FNV-1a 64-bit для строки "EventBus".
        constexpr uint64_t EXPECTED_EVENT_BUS =
            0x76cbc19a4baad9dfULL;


        // ---------------------------------------------------------------------
        // 1. Базовая проверка FNV-1a
        // ---------------------------------------------------------------------

        constexpr auto actual_empty =
            EventSystem::Core::FNV1a::hash("");

        constexpr auto actual_dummy_event_a =
            EventSystem::Core::FNV1a::hash("DummyEventA");

        constexpr auto actual_event_bus =
            EventSystem::Core::FNV1a::hash("EventBus");


        CHECK(actual_empty == EXPECTED_EMPTY_HASH);
        CHECK(actual_dummy_event_a == EXPECTED_DUMMY_EVENT_A);
        CHECK(actual_event_bus == EXPECTED_EVENT_BUS);


        // ---------------------------------------------------------------------
        // 2. Проверка DeterministicTypeRegistry
        //
        // TypeId должен определяться исключительно именем типа.
        // ---------------------------------------------------------------------

        constexpr auto registered_name =
            EventSystem::Core::DeterministicTypeRegistry::get_name<DummyEventA>();

        constexpr auto registry_id =
            EventSystem::Core::DeterministicTypeRegistry::get_id<DummyEventA>();

        constexpr auto expected_registry_id =
            EventSystem::Core::FNV1a::hash(registered_name);


        CHECK(registered_name == "DummyEventA");

        CHECK(registry_id == expected_registry_id);

        CHECK(registry_id == EXPECTED_DUMMY_EVENT_A);
    }


}

// =============================================================================
// МОДУЛЬ 2: DeterministicTypeRegistry & EventSchema
// =============================================================================
TEST_SUITE("Core::DeterministicTypeRegistry & Schema") {

    TEST_CASE("2.1. Извлечение имени типа и получение TypeId") {
        constexpr auto nameA = EventSystem::Core::DeterministicTypeRegistry::get_name<DummyEventA>();
        constexpr auto nameB = EventSystem::Core::DeterministicTypeRegistry::get_name<DummyEventB>();

        CHECK(nameA == "DummyEventA");
        CHECK(nameB == "DummyEventB");

        constexpr auto idA = EventSystem::Core::DeterministicTypeRegistry::get_id<DummyEventA>();
        constexpr auto idB = EventSystem::Core::DeterministicTypeRegistry::get_id<DummyEventB>();

        CHECK(idA != idB);
        CHECK(idA == EventSystem::Core::DeterministicTypeRegistry::get_id<DummyEventA>());
    }

    TEST_CASE("2.2. Проверка EventSchema") {
        using Schema = EventSystem::Storage::EventSchema<ComplexEvent>;

        CHECK(Schema::type_id == EventSystem::Core::DeterministicTypeRegistry::get_id<ComplexEvent>());
        CHECK(Schema::size == sizeof(ComplexEvent));
        CHECK(Schema::alignment == alignof(ComplexEvent));
    }
}

// =============================================================================
// МОДУЛЬ 3: SoAEventBucket (SoA-хранилище)
// =============================================================================
TEST_SUITE("Storage::SoAEventBucket") {

    TEST_CASE("3.1. Базовые состояния, reserve и allocated_bytes") {
        TestBucketSoA bucket;

        CHECK(bucket.size() == 0);
        CHECK(bucket.capacity() == 0);
        CHECK(bucket.allocated_bytes() == 0);

        bucket.reserve(100);
        CHECK(bucket.capacity() >= 100);
        
        std::size_t expected_min_bytes = 100 * (sizeof(int) + sizeof(float) + sizeof(std::string));
        CHECK(bucket.allocated_bytes() >= expected_min_bytes);
    }

    TEST_CASE("3.2. Добавление LValue/RValue и проверка данных") {
        TestBucketSoA bucket;

        std::string text = "LValueText";
        bucket.push(1, 1.5f, text);
        bucket.push(2, 2.5f, std::string("RValueText"));

        CHECK(bucket.size() == 2);

        auto stream0 = bucket.get_stream<0>();
        auto stream1 = bucket.get_stream<1>();
        auto stream2 = bucket.get_stream<2>();

        CHECK(stream0[0] == 1);
        CHECK(stream0[1] == 2);
        CHECK(stream1[0] == doctest::Approx(1.5f));
        CHECK(stream2[0] == "LValueText");
        CHECK(stream2[1] == "RValueText");
    }

    TEST_CASE("3.3. Поддержка Move-Only типов") {
        TestBucketMoveOnly bucket;

        auto ptr = std::make_unique<int>(42);
        bucket.push(std::move(ptr));

        CHECK(bucket.size() == 1);
        auto stream = bucket.get_stream<0>();
        REQUIRE(stream[0] != nullptr);
        CHECK(*stream[0] == 42);
    }

    TEST_CASE("3.4. Очистка и повторные вызовы clear") {
        TestBucketSoA bucket;
        bucket.push(10, 10.0f, std::string("Test"));
        
        REQUIRE(bucket.size() == 1);
        
        bucket.clear();
        CHECK(bucket.size() == 0);
        CHECK(bucket.capacity() > 0);

        bucket.clear();
        CHECK(bucket.size() == 0);
    }

    // -------------------------------------------------------------------------
    // Рандомный стресс-тест на целостность данных в SoA
    // -------------------------------------------------------------------------
    TEST_CASE("3.5. Рандомный стресс-тест SoA-структуры") {
        TestBucketStress bucket;
        std::mt19937_64 rng(1337); // Фиксированный seed для воспроизодимости результатов

        std::uniform_int_distribution<uint64_t> dist_u64(0, 1000000);
        std::uniform_real_distribution<double> dist_dbl(-1000.0, 1000.0);
        std::uniform_int_distribution<int32_t> dist_i32(-500, 500);

        struct EventData {
            uint64_t a;
            double b;
            int32_t c;
        };

        constexpr size_t TEST_COUNT = 5000;
        std::vector<EventData> reference_data;
        reference_data.reserve(TEST_COUNT);

        // Заполняем сгенерированными случайными данными
        for (size_t i = 0; i < TEST_COUNT; ++i) {
            EventData data{dist_u64(rng), dist_dbl(rng), dist_i32(rng)};
            reference_data.push_back(data);
            bucket.push(data.a, data.b, data.c);
        }

        CHECK(bucket.size() == TEST_COUNT);

        auto s0 = bucket.get_stream<0>();
        auto s1 = bucket.get_stream<1>();
        auto s2 = bucket.get_stream<2>();

        // Валидируем совпадение последовательностей SoA и эталона
        bool all_valid = true;
        for (size_t i = 0; i < TEST_COUNT; ++i) {
            if (s0[i] != reference_data[i].a ||
                s1[i] != doctest::Approx(reference_data[i].b) ||
                s2[i] != reference_data[i].c) {
                all_valid = false;
                break;
            }
        }
        CHECK(all_valid);
    }
}

// =============================================================================
// МОДУЛЬ 4: EventBus (Шина событий)
// =============================================================================
TEST_SUITE("Bus::EventBus") {

    TEST_CASE("4.1. Регистрация и получение зарегистрированных бакетов") {
        EventSystem::Bus::EventBus bus;

        CHECK(bus.get_bucket<TestBucketSoA>() == nullptr);

        bus.register_event<TestBucketSoA>(10);
        
        auto* bucket = bus.get_bucket<TestBucketSoA>();
        REQUIRE(bucket != nullptr);
        CHECK(bucket->capacity() >= 10);
    }

    TEST_CASE("4.2. Повторная регистрация того же бакета") {
        EventSystem::Bus::EventBus bus;

        bus.register_event<TestBucketSoA>(10);
        bus.register_event<TestBucketSoA>(50);
        
        auto* bucket = bus.get_bucket<TestBucketSoA>();
        REQUIRE(bucket != nullptr);
    }

    TEST_CASE("4.3. Emit в зарегистрированный и незарегистрированный бакеты") {
        EventSystem::Bus::EventBus bus;

        bus.register_event<TestBucketSoA>(10);

        bus.emit<TestBucketSoA>(100, 3.14f, std::string("EventBusTest"));
        
        auto* bucket = bus.get_bucket<TestBucketSoA>();
        REQUIRE(bucket != nullptr);
        CHECK(bucket->size() == 1);

        bus.emit<TestBucketCollision>(1u, 2u);
        CHECK(bus.get_bucket<TestBucketCollision>() == nullptr);
    }

    TEST_CASE("4.4. Очистка всей шины (clear_all)") {
        EventSystem::Bus::EventBus bus;

        bus.clear_all();

        bus.register_event<TestBucketSoA>();
        bus.register_event<TestBucketCollision>();

        bus.emit<TestBucketSoA>(1, 1.0f, std::string("Data"));
        bus.emit<TestBucketCollision>(5u, 10u);

        bus.clear_all();

        auto* bucketA = bus.get_bucket<TestBucketSoA>();
        auto* bucketB = bus.get_bucket<TestBucketCollision>();

        REQUIRE(bucketA != nullptr);
        REQUIRE(bucketB != nullptr);

        CHECK(bucketA->size() == 0);
        CHECK(bucketB->size() == 0);
    }
}