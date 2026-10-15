// Device на драйвере SDL «dummy»: звука нет, но поток вывода работает и тянет кадры у микшера.

#include <AudioSystem/Device.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace AudioSystem;

TEST_CASE("Device (dummy): поток вывода берёт кадры у микшера") {
#if defined(_WIN32)
    _putenv_s("SDL_AUDIODRIVER", "dummy");
#else
    setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
    auto device = Device::create({.sample_rate = 48000, .buffer_frames = 256});
    if (!device) {
        MESSAGE("audio device is not available: " << device.error());
        return;
    }
    CHECK(device->sample_rate() == 48000);
    CHECK(device->driver_name() == "dummy");

    const VoiceId id = device->play_note({.frequency = 440.0f}, 0.05f);
    CHECK(id);
    bool ran = false;
    for (int i = 0; i < 100 && !ran; ++i) { // до секунды: dummy-драйвер потребляет данные в реальном времени
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ran = device->frames_rendered() > 2000;
    }
    CHECK(ran);
    device->with_mixer([](Mixer& m) { m.set_master_volume(0.5f); });
    device->stop_all();
    CHECK(device->active_voices() == 0);
    device->pause();
    device->resume();
}
