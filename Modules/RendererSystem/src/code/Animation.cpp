#include <RendererSystem/Animation/Animation.hpp>
#include <RendererSystem/Core/Error.hpp>

#include <cmath>
#include <format>

namespace RendererSystem {

float AnimationClip::total_duration() const noexcept {
    float total = 0.0f;
    for (const AnimationFrame& frame : frames) {
        total += frame.duration;
    }
    return total;
}

std::vector<AnimationFrame> make_grid_frames(SpriteSheetGrid grid, int first, int count, float duration) {
    if (grid.columns <= 0 || grid.rows <= 0) {
        throw RendererError(std::format("make_grid_frames: invalid grid {}x{}", grid.columns, grid.rows));
    }
    if (first < 0 || count <= 0 || first + count > grid.columns * grid.rows) {
        throw RendererError(std::format("make_grid_frames: frames [{}, {}) are outside the {}x{} grid", first,
                                        first + count, grid.columns, grid.rows));
    }

    const float cell_w = 1.0f / static_cast<float>(grid.columns);
    const float cell_h = 1.0f / static_cast<float>(grid.rows);

    std::vector<AnimationFrame> frames;
    frames.reserve(static_cast<std::size_t>(count));
    for (int i = first; i < first + count; ++i) {
        const auto column = static_cast<float>(i % grid.columns);
        const auto row = static_cast<float>(i / grid.columns);
        frames.push_back(AnimationFrame{
            .uv = UvRect{{column * cell_w, row * cell_h}, {(column + 1.0f) * cell_w, (row + 1.0f) * cell_h}},
            .duration = duration,
        });
    }
    return frames;
}

ClipId AnimationLibrary::add(AnimationClip clip) {
    if (clip.name.empty()) {
        throw RendererError("AnimationLibrary: clip name is empty");
    }
    if (find(clip.name)) {
        throw RendererError(std::format("AnimationLibrary: clip '{}' already exists", clip.name));
    }
    if (clip.frames.empty()) {
        throw RendererError(std::format("AnimationLibrary: clip '{}' has no frames", clip.name));
    }
    for (const AnimationFrame& frame : clip.frames) {
        if (!(frame.duration > 0.0f) || !std::isfinite(frame.duration)) {
            throw RendererError(std::format("AnimationLibrary: clip '{}' has a frame with duration {}", clip.name,
                                            frame.duration));
        }
    }

    m_durations.push_back(clip.total_duration());
    m_clips.push_back(std::move(clip));
    return ClipId{static_cast<std::uint32_t>(m_clips.size() - 1)};
}

std::optional<ClipId> AnimationLibrary::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].name == name) {
            return ClipId{static_cast<std::uint32_t>(i)};
        }
    }
    return std::nullopt;
}

const AnimationClip& AnimationLibrary::clip(ClipId id) const {
    if (!contains(id)) {
        throw RendererError("AnimationLibrary: clip id does not belong to this library");
    }
    return m_clips[id.index];
}

void advance_animations(std::span<AnimationState> states, const AnimationLibrary& library, float dt) {
    for (AnimationState& state : states) {
        if (!library.contains(state.clip) || state.speed <= 0.0f) {
            continue;
        }
        const AnimationClip& clip = library[state.clip];
        const auto last = static_cast<std::uint32_t>(clip.frames.size() - 1);

        float time = state.time + dt * state.speed;

        // Целые обороты зацикленного клипа не меняют кадр — отбрасываем их сразу,
        // чтобы огромный dt (например после паузы) не крутил цикл тысячи раз.
        if (clip.looping) {
            const float total = library.duration(state.clip);
            if (time >= total) {
                time = std::fmod(time, total);
            }
        }

        while (time >= clip.frames[state.frame].duration) {
            if (state.frame < last) {
                time -= clip.frames[state.frame].duration;
                ++state.frame;
            } else if (clip.looping) {
                time -= clip.frames[state.frame].duration;
                state.frame = 0;
            } else {
                time = clip.frames[state.frame].duration; // остановились на последнем кадре
                break;
            }
        }
        state.time = time;
    }
}

UvRect current_uv(const AnimationState& state, const AnimationLibrary& library) noexcept {
    if (!library.contains(state.clip)) {
        return UvRect{};
    }
    return library[state.clip].frames[state.frame].uv;
}

bool is_finished(const AnimationState& state, const AnimationLibrary& library) noexcept {
    if (!library.contains(state.clip)) {
        return false;
    }
    const AnimationClip& clip = library[state.clip];
    return !clip.looping && state.frame + 1 == clip.frames.size() && state.time >= clip.frames.back().duration;
}

} // namespace RendererSystem
