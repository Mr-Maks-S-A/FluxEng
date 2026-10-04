/**
 * @file 02_lifecycle_and_errors.cpp
 * @brief Жизненный цикл модулей: порядок хуков, ошибки зависимостей, откат при сбое init().
 */

#include <RuntimeSystem/RuntimeSystem.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rs = RuntimeSystem;

class Step final : public rs::Module {
public:
    Step(std::string name, std::string dependency = {}, bool fail_init = false)
        : m_name(std::move(name)), m_fail(fail_init) {
        if (!dependency.empty()) depends_on(dependency);
    }
    std::string_view name() const noexcept override { return m_name; }
    void declare(rs::Runtime&) override { std::printf("  %s.declare\n", m_name.c_str()); }
    void init(rs::Runtime&) override {
        std::printf("  %s.init\n", m_name.c_str());
        if (m_fail) throw std::runtime_error(m_name + ": нет места под ресурсы");
    }
    void shutdown(rs::Runtime&) noexcept override { std::printf("  %s.shutdown\n", m_name.c_str()); }

private:
    std::string m_name;
    bool m_fail;
};

int main() {
    std::printf("1. Нормальный запуск: Net зависит от Core, World — от Net.\n");
    {
        rs::Runtime rt({.threads = 0});
        rt.add<Step>("World", "Net");
        rt.add<Step>("Net", "Core");
        rt.add<Step>("Core");
        rt.initialize(); // declare всех, затем init по зависимостям
        std::printf("  -- работаем --\n");
    } // ~Runtime: shutdown в обратном порядке

    std::printf("2. Цикл зависимостей — ошибка до первого хука:\n");
    {
        rs::Runtime rt({.threads = 0});
        rt.add<Step>("A", "B");
        rt.add<Step>("B", "A");
        try {
            rt.initialize();
        } catch (const rs::RuntimeError& error) {
            std::printf("  RuntimeError: %s\n", error.what());
        }
    }

    std::printf("3. init() бросил: поднятое откатывается, последующее не запускается:\n");
    {
        rs::Runtime rt({.threads = 0});
        rt.add<Step>("Core");
        rt.add<Step>("Net", "Core");
        rt.add<Step>("Voxel", "Net", /*fail_init=*/true);
        rt.add<Step>("Magic", "Voxel");
        try {
            rt.initialize();
        } catch (const std::exception& error) {
            std::printf("  исключение: %s (фаза Stopped: %s)\n", error.what(), rt.phase() == rs::Phase::Stopped ? "да" : "нет");
        }
    }
    return 0;
}
