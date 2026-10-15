# AudioSystem — звук

Три слоя, платформа только в последнем:

| Слой | Что | Платформа |
|---|---|---|
| `Wave` | PCM-буфер (моно/стерео, float), `make_tone` (синус, квадрат, пила, треугольник, шум), `make_noise_burst`, чтение и запись WAV (PCM 8/16/24/32, float32) | нет |
| `Mixer` | голоса: сэмплы (питч, панорама, цикл) и ноты-синтезаторы (ADSR, вибрато, PolyBLEP для квадрата и пилы); `render()` без выделений памяти | нет |
| `Device` | вывод через SDL3 (`SDL_OpenAudioDeviceStream`), микшер под коротким замком | SDL3 |

Подключение: `#include <AudioSystem/AudioSystem.hpp>`, цель `engine::AudioSystem` (входит в `engine::Platform`).

```cpp
auto device = AudioSystem::Device::create();                       // нет устройства → ошибка, игра идёт без звука
auto hit = std::make_shared<AudioSystem::Wave>(AudioSystem::make_noise_burst(0.1f));
device->play(hit, {.volume = 0.8f, .pan = -0.3f});
auto note = device->note_on({.shape = AudioSystem::Shape::Saw, .frequency = AudioSystem::midi_to_hz(60)});
device->release(note);                                             // затухание по ADSR
```

**Микшер — чистая функция времени.** Одни и те же команды дают одни и те же отсчёты (тест на детерминизм и на независимость от
размера блоков), поэтому звук проверяется без звуковой карты, а `Sandbox/sound_pad --wav` рендерит мелодию прямо в файл.

В тестах и CI: `SDL_AUDIODRIVER=dummy` — устройство без звука, время идёт, поток вывода тянет кадры у микшера.

Тесты: `AudioSystemTests`. Проект со звуком: `Sandbox/sound_pad` (SoundPad).
