#include <WindowSystem/Listeners.hpp>

#include <doctest/doctest.h>

#include <vector>

using WindowSystem::ListenerId;
using WindowSystem::Listeners;

TEST_SUITE("WindowSystem.Listeners") {

TEST_CASE("ListenerId{} — «нет подписки» (ZII)") {
    Listeners<int> listeners;
    CHECK_FALSE(ListenerId{});
    CHECK_FALSE(listeners.unsubscribe(ListenerId{}));
    CHECK_FALSE(listeners.subscribe(nullptr));
    CHECK(listeners.empty());
}

TEST_CASE("несколько подписчиков вызываются в порядке подписки") {
    Listeners<int> listeners;
    std::vector<int> calls;
    listeners.subscribe([&](int v) { calls.push_back(v * 1); });
    const ListenerId second = listeners.subscribe([&](int v) { calls.push_back(v * 10); });
    listeners.subscribe([&](int v) { calls.push_back(v * 100); });
    listeners.emit(2);
    CHECK(calls == std::vector<int>{2, 20, 200});

    calls.clear();
    CHECK(listeners.unsubscribe(second));
    CHECK_FALSE(listeners.unsubscribe(second));
    listeners.emit(1);
    CHECK(calls == std::vector<int>{1, 100});
    CHECK(listeners.size() == 2);
}

TEST_CASE("отписка изнутри обработчика") {
    Listeners<> listeners;
    int a = 0;
    int b = 0;
    ListenerId self{};
    self = listeners.subscribe([&] {
        ++a;
        listeners.unsubscribe(self);
    });
    listeners.subscribe([&] { ++b; });
    listeners.emit();
    listeners.emit();
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(listeners.size() == 1);
}

TEST_CASE("подписка изнутри обработчика получает уже следующее событие") {
    Listeners<> listeners;
    int late = 0;
    listeners.subscribe([&] {
        if (listeners.size() == 1) listeners.subscribe([&] { ++late; });
    });
    listeners.emit();
    CHECK(late == 0);
    listeners.emit();
    CHECK(late == 1);
}

TEST_CASE("clear во время рассылки") {
    Listeners<> listeners;
    int calls = 0;
    listeners.subscribe([&] {
        ++calls;
        listeners.clear();
    });
    listeners.subscribe([&] { ++calls; });
    listeners.emit();
    CHECK(calls == 1);
    CHECK(listeners.empty());
}

}
