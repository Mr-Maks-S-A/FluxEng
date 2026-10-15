#include <AudioSystem/Device.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

namespace AudioSystem {

struct Device::Impl {
    Mixer mixer;
    std::mutex mutex;
    SDL_AudioStream* stream = nullptr;
    std::vector<float> scratch;

    Impl(int rate, std::size_t voices) : mixer(rate, voices) {}

    ~Impl() {
        if (stream != nullptr) SDL_DestroyAudioStream(stream); // закрывает и устройство, открытое для потока
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    /// Поток звука SDL просит добавить данных: микшер отдаёт ровно столько кадров, сколько не хватает.
    static void SDLCALL on_need_data(void* userdata, SDL_AudioStream* sdl_stream, int additional_amount, int /*total_amount*/) {
        auto* self = static_cast<Impl*>(userdata);
        if (additional_amount <= 0) return;
        const auto samples = static_cast<std::size_t>(additional_amount) / sizeof(float);
        const std::size_t even = samples & ~static_cast<std::size_t>(1);
        if (self->scratch.size() < even) self->scratch.resize(even);
        {
            std::lock_guard lock(self->mutex);
            self->mixer.render(std::span<float>(self->scratch.data(), even));
        }
        SDL_PutAudioStreamData(sdl_stream, self->scratch.data(), static_cast<int>(even * sizeof(float)));
    }
};

Device::Lock::Lock(Impl* i) : impl(i) { impl->mutex.lock(); }
Device::Lock::~Lock() { impl->mutex.unlock(); }

Device::Device(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;
Device::~Device() = default;

std::expected<Device, std::string> Device::create(const DeviceConfig& config) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return std::unexpected(std::format("cannot initialize SDL audio: {}", SDL_GetError()));

    auto impl = std::make_unique<Impl>(config.sample_rate, config.max_voices);
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = config.sample_rate;
    if (config.buffer_frames > 0) {
        const std::string frames = std::to_string(config.buffer_frames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames.c_str());
    }
    impl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::on_need_data, impl.get());
    if (impl->stream == nullptr) return std::unexpected(std::format("cannot open the audio device: {}", SDL_GetError()));
    if (!SDL_ResumeAudioStreamDevice(impl->stream)) return std::unexpected(std::format("cannot start the audio device: {}", SDL_GetError()));
    return Device(std::move(impl));
}

int Device::sample_rate() const noexcept { return m_impl->mixer.sample_rate(); }

std::string Device::driver_name() const {
    const char* name = SDL_GetCurrentAudioDriver();
    return name != nullptr ? name : "";
}

void Device::pause() noexcept { SDL_PauseAudioStreamDevice(m_impl->stream); }
void Device::resume() noexcept { SDL_ResumeAudioStreamDevice(m_impl->stream); }

Mixer& Device::mixer() noexcept { return m_impl->mixer; }

VoiceId Device::play(std::shared_ptr<const Wave> wave, PlayParams params) {
    return with_mixer([&](Mixer& m) { return m.play(std::move(wave), params); });
}
VoiceId Device::note_on(const ToneParams& params) {
    return with_mixer([&](Mixer& m) { return m.note_on(params); });
}
VoiceId Device::play_note(const ToneParams& params, float hold_seconds) {
    return with_mixer([&](Mixer& m) { return m.play_note(params, hold_seconds); });
}
void Device::release(VoiceId id) noexcept {
    std::lock_guard lock(m_impl->mutex);
    m_impl->mixer.release(id);
}
void Device::stop_all() noexcept {
    std::lock_guard lock(m_impl->mutex);
    m_impl->mixer.stop_all();
}
void Device::set_master_volume(float volume) noexcept {
    std::lock_guard lock(m_impl->mutex);
    m_impl->mixer.set_master_volume(volume);
}
std::size_t Device::active_voices() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->mixer.active_voices();
}
std::uint64_t Device::frames_rendered() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->mixer.frames_rendered();
}

} // namespace AudioSystem
