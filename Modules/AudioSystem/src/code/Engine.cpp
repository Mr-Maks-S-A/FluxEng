#include <AudioSystem/Dsp.hpp>
#include <AudioSystem/Engine.hpp>
#include <AudioSystem/Spatial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace AudioSystem {

std::vector<BusConfig> default_buses() {
    return {
        {"master", 0, 1.0f}, {"sfx", 0, 1.0f}, {"music", 0, 1.0f}, {"voice", 0, 1.0f}, {"ambient", 0, 1.0f}, {"ui", 0, 1.0f},
    };
}

namespace {

constexpr std::size_t max_buses = 32;
constexpr float no_filter_hz = 19'900.0f;

enum class Cmd : std::uint8_t {
    Play, Stop, StopBus, StopAll, Pause, VoiceVolume, VoicePitch, VoicePan, VoiceSpatial, VoicePosition,
    BusVolume, BusMute, BusLowpass, BusPause, SetListener, SetReverb,
};

struct Command {
    Cmd type = Cmd::Stop;
    std::uint32_t id = 0;       // голос
    std::uint16_t bus = 0;
    bool flag = false;
    float a = 0.0f;             // громкость / питч / пан / секунды
    float b = 0.0f;             // секунды рампы
    PlayParams play{};          // Play; для VoiceSpatial — play.spatial; для VoicePosition — spatial.position/velocity
    Listener listener{};
    ReverbParams reverb{};
    std::shared_ptr<const AudioClip> clip;
};

struct Voice {
    std::uint32_t id = 0;
    std::shared_ptr<const AudioClip> clip;
    double position = 0.0;
    std::uint32_t delay_frames = 0;
    PlayParams params{};
    Ramp volume{};
    Ramp fade{};
    bool stopping = false;
    bool paused = false;
    float last_gain_l = 0.0f;
    float last_gain_r = 0.0f;
    float loudness = 0.0f;
    LowPass lp_left;
    LowPass lp_right;
};

struct Bus {
    std::uint16_t parent = 0;
    Ramp volume{};
    Ramp mute = Ramp::at(1.0f);
    bool paused = false;
    bool effective_paused = false;
    float cutoff = 20'000.0f;
    LowPass lp_left;
    LowPass lp_right;
    float duck = 1.0f;
    float last_gain = 1.0f;
    std::vector<float> left;
    std::vector<float> right;
};

float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

struct AudioEngine::Impl {
    // ---------------------------------------------------------------- управляющая сторона (под мьютексом)
    struct ClipEntry {
        std::string name;
        std::shared_ptr<const AudioClip> clip;
    };
    mutable std::mutex control_mutex;
    std::vector<ClipEntry> clips;
    std::map<std::string, std::uint32_t, std::less<>> clip_by_name;
    std::uint32_t next_voice = 1;

    // Кольцо SPSC: писатель (под мьютексом) двигает tail, аудиопоток — head.
    std::vector<Command> ring;
    std::size_t ring_mask = 0;
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};

    // ---------------------------------------------------------------- аудио-сторона (владеет render)
    std::uint32_t sample_rate;
    std::uint32_t block;
    std::vector<Voice> voices;
    std::vector<Bus> buses;
    std::vector<DuckRule> rules;
    std::vector<float> rule_gain;
    Listener listener{};
    Reverb reverb;
    Limiter limiter;
    std::vector<float> send_left, send_right, rev_left, rev_right;

    // ---------------------------------------------------------------- опубликованное для чтения из других потоков
    std::vector<std::atomic<std::uint32_t>> active_ids;
    std::atomic<std::uint32_t> processed_voice{0};
    std::vector<std::atomic<float>> meter_peak;
    std::vector<std::atomic<float>> meter_rms;
    std::atomic<std::uint64_t> frames_rendered{0};
    std::atomic<std::uint64_t> dropped_commands{0};
    std::atomic<std::uint64_t> dropped_voices{0};
    std::atomic<std::uint64_t> stolen_voices{0};
    std::atomic<std::uint64_t> limiter_blocks{0};

    explicit Impl(const EngineConfig& cfg)
        : sample_rate(cfg.sample_rate), block(cfg.block_frames), voices(cfg.max_voices), buses(cfg.buses.size()), rules(cfg.ducking),
          reverb(cfg.sample_rate), limiter(cfg.sample_rate, cfg.limiter_ceiling),
          active_ids(cfg.max_voices), meter_peak(cfg.buses.size()), meter_rms(cfg.buses.size()) {
        std::size_t capacity = 64;
        while (capacity < cfg.command_capacity) capacity <<= 1;
        ring.resize(capacity);
        ring_mask = capacity - 1;
        for (std::size_t i = 0; i < buses.size(); ++i) {
            buses[i].parent = cfg.buses[i].parent;
            buses[i].volume = Ramp::at(cfg.buses[i].volume);
            buses[i].left.assign(block, 0.0f);
            buses[i].right.assign(block, 0.0f);
        }
        rule_gain.assign(rules.size(), 1.0f);
        send_left.assign(block, 0.0f);
        send_right.assign(block, 0.0f);
        rev_left.assign(block, 0.0f);
        rev_right.assign(block, 0.0f);
        for (auto& a : active_ids) a.store(0);
        for (auto& m : meter_peak) m.store(0.0f);
        for (auto& m : meter_rms) m.store(0.0f);
    }

    // ================================================================ очередь

    bool push(Command&& command) {
        const std::uint64_t t = tail.load(std::memory_order_relaxed);
        if (t - head.load(std::memory_order_acquire) >= ring.size()) {
            dropped_commands.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ring[t & ring_mask] = std::move(command);
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool is_descendant(std::uint16_t bus, std::uint16_t ancestor) const noexcept {
        for (std::size_t guard = 0; guard < buses.size(); ++guard) {
            if (bus == ancestor) return true;
            if (bus == 0) return false;
            bus = buses[bus].parent;
        }
        return false;
    }

    Voice* find_voice(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        for (Voice& v : voices) if (v.id == id) return &v;
        return nullptr;
    }

    void free_voice(std::size_t slot) noexcept {
        voices[slot].id = 0;
        voices[slot].clip.reset();
        active_ids[slot].store(0, std::memory_order_release);
    }

    void begin_stop(Voice& v, float seconds) noexcept {
        v.stopping = true;
        v.fade.set(0.0f, seconds, sample_rate);
    }

    // ================================================================ команды

    void apply_play(Command& c) {
        const std::uint32_t id = c.id;
        std::size_t slot = voices.size();
        for (std::size_t i = 0; i < voices.size(); ++i) if (voices[i].id == 0) { slot = i; break; }
        if (slot == voices.size()) {
            // Пул занят: вытесняем самый неважный (низкий приоритет → тише → старше), если новый не менее важен.
            std::size_t victim = 0;
            for (std::size_t i = 1; i < voices.size(); ++i) {
                const Voice& a = voices[i];
                const Voice& b = voices[victim];
                if (a.params.priority != b.params.priority ? a.params.priority < b.params.priority
                    : a.loudness != b.loudness            ? a.loudness < b.loudness
                                                          : a.id < b.id) victim = i;
            }
            if (!voices.empty() && voices[victim].params.priority <= c.play.priority) {
                slot = victim;
                stolen_voices.fetch_add(1, std::memory_order_relaxed);
            } else {
                dropped_voices.fetch_add(1, std::memory_order_relaxed);
                processed_voice.store(id, std::memory_order_release);
                return;
            }
        }

        Voice& v = voices[slot];
        v = Voice{};
        v.id = id;
        v.clip = std::move(c.clip);
        v.params = c.play;
        v.params.bus.index = c.play.bus.index < buses.size() ? c.play.bus.index : 1;
        v.volume = Ramp::at(v.params.volume);
        v.fade = Ramp::at(1.0f);
        if (v.params.fade_in > 0.0f) { v.fade.value = 0.0f; v.fade.set(1.0f, v.params.fade_in, sample_rate); }
        v.delay_frames = static_cast<std::uint32_t>(std::max(0.0f, v.params.start_delay) * static_cast<float>(sample_rate));
        const auto frames = static_cast<double>(v.clip->frames());
        v.position = std::clamp(static_cast<double>(v.params.start_offset) * v.clip->sample_rate, 0.0, std::max(0.0, frames - 1.0));
        active_ids[slot].store(id, std::memory_order_release);
        processed_voice.store(id, std::memory_order_release);
    }

    void apply(Command& c) {
        switch (c.type) {
        case Cmd::Play: apply_play(c); break;
        case Cmd::Stop: if (Voice* v = find_voice(c.id)) begin_stop(*v, c.a); break;
        case Cmd::StopBus:
            for (Voice& v : voices) if (v.id != 0 && is_descendant(v.params.bus.index, c.bus)) begin_stop(v, c.a);
            break;
        case Cmd::StopAll: for (Voice& v : voices) if (v.id != 0) begin_stop(v, c.a); break;
        case Cmd::Pause: if (Voice* v = find_voice(c.id)) v->paused = c.flag; break;
        case Cmd::VoiceVolume: if (Voice* v = find_voice(c.id)) v->volume.set(c.a, c.b, sample_rate); break;
        case Cmd::VoicePitch: if (Voice* v = find_voice(c.id)) v->params.pitch = c.a; break;
        case Cmd::VoicePan: if (Voice* v = find_voice(c.id)) v->params.pan = c.a; break;
        case Cmd::VoiceSpatial: if (Voice* v = find_voice(c.id)) v->params.spatial = c.play.spatial; break;
        case Cmd::VoicePosition:
            if (Voice* v = find_voice(c.id)) {
                v->params.spatial.position = c.play.spatial.position;
                v->params.spatial.velocity = c.play.spatial.velocity;
            }
            break;
        case Cmd::BusVolume: buses[c.bus].volume.set(c.a, c.b, sample_rate); break;
        case Cmd::BusMute: buses[c.bus].mute.set(c.flag ? 0.0f : 1.0f, 0.02f, sample_rate); break;
        case Cmd::BusLowpass: buses[c.bus].cutoff = c.a; break;
        case Cmd::BusPause: buses[c.bus].paused = c.flag; break;
        case Cmd::SetListener: listener = c.listener; break;
        case Cmd::SetReverb:
            reverb.set(c.reverb);
            if (c.reverb.wet <= 0.0f) reverb.clear();
            break;
        }
    }

    void drain() {
        std::uint64_t h = head.load(std::memory_order_relaxed);
        const std::uint64_t t = tail.load(std::memory_order_acquire);
        for (; h < t; ++h) apply(ring[h & ring_mask]);
        head.store(h, std::memory_order_release);
    }

    // ================================================================ смешивание голоса

    /// Возвращает false, когда голос закончился.
    bool mix_voice(Voice& v, std::uint32_t n, bool reverb_on) {
        Bus& bus = buses[v.params.bus.index];
        std::uint32_t first = 0;
        if (v.delay_frames > 0) {
            if (v.delay_frames >= n) { v.delay_frames -= n; return true; }
            first = v.delay_frames;
            v.delay_frames = 0;
        }
        const std::uint32_t count = n - first;
        const AudioClip& clip = *v.clip;
        const bool halted = v.paused || bus.effective_paused;

        // Рампы голоса идут только пока он играет.
        float vol = 0.0f;
        if (!halted) {
            v.volume.advance(count);
            v.fade.advance(count);
        }
        vol = v.volume.value * v.fade.value;

        float pitch = std::clamp(v.params.pitch, 0.01f, 16.0f);
        float pan = v.params.pan;
        float attenuation_gain = 1.0f;
        float cutoff = 20'000.0f;
        const bool spatial = v.params.spatial.enabled;
        if (spatial) {
            const SpatialResult sr = compute_spatial(v.params.spatial, listener);
            pitch = std::clamp(pitch * sr.pitch, 0.01f, 16.0f);
            pan = sr.pan;
            attenuation_gain = sr.gain;
            cutoff = sr.cutoff_hz;
        }

        const bool stereo_source = clip.channels == 2 && !spatial;
        float target_l, target_r;
        if (stereo_source) {
            const float p = std::clamp(pan, -1.0f, 1.0f);
            target_l = vol * attenuation_gain * (p > 0.0f ? 1.0f - p : 1.0f);
            target_r = vol * attenuation_gain * (p < 0.0f ? 1.0f + p : 1.0f);
        } else {
            const PanGains pg = pan_constant_power(pan);
            target_l = vol * attenuation_gain * pg.left;
            target_r = vol * attenuation_gain * pg.right;
        }
        if (halted) target_l = target_r = 0.0f;
        v.loudness = std::max(target_l, target_r);

        if (halted && v.last_gain_l == 0.0f && v.last_gain_r == 0.0f) return true; // уже стих: позиция стоит

        const bool use_lp = cutoff < no_filter_hz;
        if (use_lp) { v.lp_left.set_cutoff(cutoff, sample_rate); v.lp_right.set_cutoff(cutoff, sample_rate); }
        else { v.lp_left.reset(); v.lp_right.reset(); }

        const std::uint64_t frames = clip.frames();
        const std::uint64_t loop_begin = clip.effective_loop_begin();
        const std::uint64_t loop_end = clip.effective_loop_end();
        const bool looping = v.params.loop;
        const double step = static_cast<double>(pitch) * clip.sample_rate / sample_rate;
        const double saved_position = v.position;
        const std::uint32_t channels = clip.channels;
        const float* data = clip.samples.data();
        const float send = reverb_on ? clamp01(v.params.reverb_send) : 0.0f;

        const auto at = [&](std::int64_t index, std::uint32_t channel) noexcept -> float {
            if (index < 0) index = 0;
            auto i = static_cast<std::uint64_t>(index);
            if (i >= loop_end && looping) i = loop_begin + (i - loop_end) % (loop_end - loop_begin);
            if (i >= frames) return 0.0f;
            return data[i * channels + channel];
        };

        bool alive = true;
        float* out_l = bus.left.data();
        float* out_r = bus.right.data();
        const float l0 = v.last_gain_l, r0 = v.last_gain_r;
        const float inv_count = 1.0f / static_cast<float>(count);
        for (std::uint32_t i = first; i < n; ++i) {
            const float t = static_cast<float>(i - first + 1) * inv_count;
            const float gl = l0 + (target_l - l0) * t;
            const float gr = r0 + (target_r - r0) * t;

            const auto index = static_cast<std::int64_t>(std::floor(v.position));
            const auto frac = static_cast<float>(v.position - static_cast<double>(index));
            float left = hermite(frac, at(index - 1, 0), at(index, 0), at(index + 1, 0), at(index + 2, 0));
            float right = left;
            if (channels == 2) right = hermite(frac, at(index - 1, 1), at(index, 1), at(index + 1, 1), at(index + 2, 1));
            if (!stereo_source) {
                left = right = channels == 2 ? 0.5f * (left + right) : left;
            }
            if (use_lp) { left = v.lp_left.process(left); right = v.lp_right.process(right); }

            const float ol = left * gl;
            const float or_ = right * gr;
            out_l[i] += ol;
            out_r[i] += or_;
            if (send > 0.0f) { send_left[i] += ol * send; send_right[i] += or_ * send; }

            v.position += step;
            if (v.position >= static_cast<double>(loop_end)) {
                if (looping) {
                    const double span = static_cast<double>(loop_end - loop_begin);
                    v.position = static_cast<double>(loop_begin) + std::fmod(v.position - static_cast<double>(loop_end), span);
                } else {
                    alive = false;
                    break;
                }
            }
        }
        v.last_gain_l = target_l;
        v.last_gain_r = target_r;
        if (halted) v.position = saved_position; // пауза не двигает позицию, хоть хвост громкости и доигран
        if (v.stopping && v.fade.value <= 0.0f) alive = false;
        return alive;
    }

    // ================================================================ блок

    void render_block(float* out, std::uint32_t n) noexcept {
        drain();

        for (Bus& bus : buses) {
            std::fill_n(bus.left.data(), n, 0.0f);
            std::fill_n(bus.right.data(), n, 0.0f);
        }
        const bool reverb_on = reverb.active();
        if (reverb_on) {
            std::fill_n(send_left.data(), n, 0.0f);
            std::fill_n(send_right.data(), n, 0.0f);
        }

        buses[0].effective_paused = buses[0].paused;
        for (std::size_t i = 1; i < buses.size(); ++i) buses[i].effective_paused = buses[i].paused || buses[buses[i].parent].effective_paused;

        for (std::size_t slot = 0; slot < voices.size(); ++slot) {
            Voice& v = voices[slot];
            if (v.id == 0) continue;
            if (!mix_voice(v, n, reverb_on)) free_voice(slot);
        }

        // Шины от листьев к корню: ступень громкости, срез, измерение, сложение в родителя.
        for (std::size_t i = buses.size(); i-- > 0;) {
            Bus& bus = buses[i];
            if (i == 0 && reverb_on) {
                reverb.process(send_left.data(), send_right.data(), rev_left.data(), rev_right.data(), n);
                for (std::uint32_t k = 0; k < n; ++k) { bus.left[k] += rev_left[k]; bus.right[k] += rev_right[k]; }
            }
            bus.volume.advance(n);
            bus.mute.advance(n);
            const float gain = bus.volume.value * bus.mute.value * bus.duck;
            const float g0 = bus.last_gain;
            const float inv = 1.0f / static_cast<float>(n);
            const bool use_lp = bus.cutoff < no_filter_hz;
            if (use_lp) { bus.lp_left.set_cutoff(bus.cutoff, sample_rate); bus.lp_right.set_cutoff(bus.cutoff, sample_rate); }
            else { bus.lp_left.reset(); bus.lp_right.reset(); }

            float peak = 0.0f;
            double energy = 0.0;
            bool limited = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                const float g = g0 + (gain - g0) * static_cast<float>(k + 1) * inv;
                float l = bus.left[k] * g;
                float r = bus.right[k] * g;
                if (use_lp) { l = bus.lp_left.process(l); r = bus.lp_right.process(r); }
                if (i == 0) {
                    limited |= limiter.process(l, r);
                    l = std::clamp(l, -1.0f, 1.0f);
                    r = std::clamp(r, -1.0f, 1.0f);
                    out[2 * k] = l;
                    out[2 * k + 1] = r;
                } else {
                    buses[bus.parent].left[k] += l;
                    buses[bus.parent].right[k] += r;
                }
                peak = std::max({peak, std::abs(l), std::abs(r)});
                energy += static_cast<double>(l) * l + static_cast<double>(r) * r;
            }
            bus.last_gain = gain;
            meter_peak[i].store(peak, std::memory_order_relaxed);
            meter_rms[i].store(static_cast<float>(std::sqrt(energy / (2.0 * n))), std::memory_order_relaxed);
            if (limited) limiter_blocks.fetch_add(1, std::memory_order_relaxed);
        }

        update_ducking(n);
        frames_rendered.fetch_add(n, std::memory_order_relaxed);
    }

    void update_ducking(std::uint32_t n) noexcept {
        for (Bus& bus : buses) bus.duck = 1.0f;
        for (std::size_t r = 0; r < rules.size(); ++r) {
            const DuckRule& rule = rules[r];
            if (rule.source >= buses.size() || rule.target >= buses.size() || rule.source == rule.target) continue;
            const bool active = meter_peak[rule.source].load(std::memory_order_relaxed) > rule.threshold;
            const float target = active ? db_to_gain(-std::abs(rule.reduction_db)) : 1.0f;
            const float seconds = target < rule_gain[r] ? rule.attack_seconds : rule.release_seconds;
            const float coef = seconds <= 0.0f ? 1.0f : 1.0f - std::exp(-static_cast<float>(n) / (seconds * static_cast<float>(sample_rate)));
            rule_gain[r] += (target - rule_gain[r]) * coef;
            buses[rule.target].duck *= rule_gain[r];
        }
    }
};

// ==================================================================================================== AudioEngine

namespace {
EngineConfig sanitize(EngineConfig c) {
    c.sample_rate = std::clamp<std::uint32_t>(c.sample_rate, 8'000, 192'000);
    c.max_voices = std::clamp<std::uint32_t>(c.max_voices, 1, 1024);
    c.block_frames = std::clamp<std::uint32_t>(c.block_frames, 16, 4096);
    c.command_capacity = std::max<std::uint32_t>(c.command_capacity, 64);
    c.limiter_ceiling = std::clamp(c.limiter_ceiling, 0.1f, 1.0f);
    if (c.buses.empty() || c.buses.size() > max_buses) c.buses = default_buses();
    c.buses[0].name = c.buses[0].name.empty() ? "master" : c.buses[0].name;
    c.buses[0].parent = 0;
    for (std::size_t i = 1; i < c.buses.size(); ++i) {
        if (c.buses[i].parent >= i) c.buses[i].parent = 0; // родитель обязан стоять раньше: иначе в корень
    }
    for (BusConfig& b : c.buses) b.volume = std::max(0.0f, b.volume);
    return c;
}
} // namespace

AudioEngine::AudioEngine(EngineConfig config) : m_config(sanitize(std::move(config))), m_impl(std::make_unique<Impl>(m_config)) {}
AudioEngine::~AudioEngine() = default;

ClipId AudioEngine::add_clip(std::string name, AudioClip clip) {
    if (!clip.valid()) return {};
    std::lock_guard lock(m_impl->control_mutex);
    auto shared = std::make_shared<const AudioClip>(std::move(clip));
    if (const auto it = m_impl->clip_by_name.find(name); it != m_impl->clip_by_name.end()) {
        m_impl->clips[it->second - 1].clip = std::move(shared);
        return ClipId{it->second};
    }
    m_impl->clips.push_back({name, std::move(shared)});
    const auto id = static_cast<std::uint32_t>(m_impl->clips.size());
    m_impl->clip_by_name.emplace(std::move(name), id);
    return ClipId{id};
}

ClipId AudioEngine::find_clip(std::string_view name) const {
    std::lock_guard lock(m_impl->control_mutex);
    const auto it = m_impl->clip_by_name.find(name);
    if (it == m_impl->clip_by_name.end() || !m_impl->clips[it->second - 1].clip) return {};
    return ClipId{it->second};
}

void AudioEngine::remove_clip(ClipId id) {
    std::lock_guard lock(m_impl->control_mutex);
    if (id.value == 0 || id.value > m_impl->clips.size()) return;
    m_impl->clips[id.value - 1].clip.reset();
}

std::shared_ptr<const AudioClip> AudioEngine::clip(ClipId id) const {
    std::lock_guard lock(m_impl->control_mutex);
    if (id.value == 0 || id.value > m_impl->clips.size()) return {};
    return m_impl->clips[id.value - 1].clip;
}

VoiceId AudioEngine::play(ClipId clip_id, const PlayParams& params) {
    std::lock_guard lock(m_impl->control_mutex);
    if (clip_id.value == 0 || clip_id.value > m_impl->clips.size() || !m_impl->clips[clip_id.value - 1].clip) return {};
    Command c;
    c.type = Cmd::Play;
    c.id = m_impl->next_voice;
    c.clip = m_impl->clips[clip_id.value - 1].clip;
    c.play = params;
    c.play.volume = std::max(0.0f, params.volume);
    c.play.pitch = std::clamp(params.pitch, 0.01f, 16.0f);
    c.play.pan = std::clamp(params.pan, -1.0f, 1.0f);
    c.play.reverb_send = clamp01(params.reverb_send);
    c.play.fade_in = std::max(0.0f, params.fade_in);
    c.play.start_delay = std::max(0.0f, params.start_delay);
    c.play.start_offset = std::max(0.0f, params.start_offset);
    if (!m_impl->push(std::move(c))) return {};
    return VoiceId{m_impl->next_voice++};
}

namespace {
// Короткие помощники построения команд.
Command voice_command(Cmd type, VoiceId voice) {
    Command c;
    c.type = type;
    c.id = voice.value;
    return c;
}
} // namespace

void AudioEngine::stop(VoiceId voice, float fade_seconds) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::Stop, voice);
    c.a = std::max(0.0f, fade_seconds);
    m_impl->push(std::move(c));
}

void AudioEngine::stop_bus(BusId bus, float fade_seconds) {
    if (bus.index >= m_config.buses.size()) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::StopBus;
    c.bus = bus.index;
    c.a = std::max(0.0f, fade_seconds);
    m_impl->push(std::move(c));
}

void AudioEngine::stop_all(float fade_seconds) {
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::StopAll;
    c.a = std::max(0.0f, fade_seconds);
    m_impl->push(std::move(c));
}

void AudioEngine::pause(VoiceId voice, bool paused) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::Pause, voice);
    c.flag = paused;
    m_impl->push(std::move(c));
}

void AudioEngine::set_voice_volume(VoiceId voice, float volume, float ramp_seconds) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::VoiceVolume, voice);
    c.a = std::max(0.0f, volume);
    c.b = std::max(0.0f, ramp_seconds);
    m_impl->push(std::move(c));
}

void AudioEngine::set_voice_pitch(VoiceId voice, float pitch) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::VoicePitch, voice);
    c.a = std::clamp(pitch, 0.01f, 16.0f);
    m_impl->push(std::move(c));
}

void AudioEngine::set_voice_pan(VoiceId voice, float pan) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::VoicePan, voice);
    c.a = std::clamp(pan, -1.0f, 1.0f);
    m_impl->push(std::move(c));
}

void AudioEngine::set_spatial(VoiceId voice, const Spatial& spatial) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::VoiceSpatial, voice);
    c.play.spatial = spatial;
    m_impl->push(std::move(c));
}

void AudioEngine::set_position(VoiceId voice, Vec3 position, Vec3 velocity) {
    if (!voice) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c = voice_command(Cmd::VoicePosition, voice);
    c.play.spatial.position = position;
    c.play.spatial.velocity = velocity;
    m_impl->push(std::move(c));
}

bool AudioEngine::is_playing(VoiceId voice) const noexcept {
    if (!voice) return false;
    // Порядок важен: сначала счётчик обработанных запусков, потом список. Запуск публикует слот раньше, чем двигает счётчик.
    if (voice.value > m_impl->processed_voice.load(std::memory_order_acquire)) return true; // ещё в очереди
    for (const auto& id : m_impl->active_ids) {
        if (id.load(std::memory_order_acquire) == voice.value) return true;
    }
    return false;
}

std::uint32_t AudioEngine::active_voices() const noexcept {
    std::uint32_t n = 0;
    for (const auto& id : m_impl->active_ids) if (id.load(std::memory_order_relaxed) != 0) ++n;
    return n;
}

BusId AudioEngine::find_bus(std::string_view name) const {
    for (std::size_t i = 0; i < m_config.buses.size(); ++i) {
        if (m_config.buses[i].name == name) return BusId{static_cast<std::uint16_t>(i)};
    }
    return BusId{std::numeric_limits<std::uint16_t>::max()};
}

void AudioEngine::set_bus_volume(BusId bus, float volume, float fade_seconds) {
    if (bus.index >= m_config.buses.size()) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::BusVolume;
    c.bus = bus.index;
    c.a = std::max(0.0f, volume);
    c.b = std::max(0.0f, fade_seconds);
    m_impl->push(std::move(c));
}

void AudioEngine::set_bus_mute(BusId bus, bool muted) {
    if (bus.index >= m_config.buses.size()) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::BusMute;
    c.bus = bus.index;
    c.flag = muted;
    m_impl->push(std::move(c));
}

void AudioEngine::set_bus_lowpass(BusId bus, float cutoff_hz) {
    if (bus.index >= m_config.buses.size()) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::BusLowpass;
    c.bus = bus.index;
    c.a = std::clamp(cutoff_hz, 20.0f, 20'000.0f);
    m_impl->push(std::move(c));
}

void AudioEngine::pause_bus(BusId bus, bool paused) {
    if (bus.index >= m_config.buses.size()) return;
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::BusPause;
    c.bus = bus.index;
    c.flag = paused;
    m_impl->push(std::move(c));
}

BusMeter AudioEngine::bus_meter(BusId bus) const noexcept {
    if (bus.index >= m_config.buses.size()) return {};
    return {m_impl->meter_peak[bus.index].load(std::memory_order_relaxed), m_impl->meter_rms[bus.index].load(std::memory_order_relaxed)};
}

void AudioEngine::set_listener(const Listener& listener) {
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::SetListener;
    c.listener = listener;
    m_impl->push(std::move(c));
}

void AudioEngine::set_reverb(const ReverbParams& params) {
    std::lock_guard lock(m_impl->control_mutex);
    Command c;
    c.type = Cmd::SetReverb;
    c.reverb = params;
    m_impl->push(std::move(c));
}

void AudioEngine::render(std::span<float> buffer) noexcept {
    const auto total = static_cast<std::uint32_t>(buffer.size() / 2);
    std::uint32_t done = 0;
    while (done < total) {
        const std::uint32_t n = std::min(total - done, m_impl->block);
        m_impl->render_block(buffer.data() + 2ull * done, n);
        done += n;
    }
}

Stats AudioEngine::stats() const noexcept {
    return {m_impl->frames_rendered.load(), m_impl->dropped_commands.load(), m_impl->dropped_voices.load(), m_impl->stolen_voices.load(),
            m_impl->limiter_blocks.load()};
}

double AudioEngine::time_seconds() const noexcept {
    return static_cast<double>(m_impl->frames_rendered.load(std::memory_order_relaxed)) / m_config.sample_rate;
}

} // namespace AudioSystem
