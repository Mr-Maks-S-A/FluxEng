#pragma once
/**
 * @file AudioSystem.hpp
 * @brief Общий заголовок AudioSystem: `#include <AudioSystem/AudioSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | AudioSystem::Vec3, ClipId, VoiceId, BusId, PlayParams, Spatial, Listener, ReverbParams | Types.hpp |
 * | AudioSystem::AudioClip, decode_wav / decode_ogg / decode_audio, encode_wav, make_sine… | Clip.hpp |
 * | AudioSystem::Ramp, LowPass, Reverb, Limiter, hermite, pan_constant_power | Dsp.hpp |
 * | AudioSystem::compute_spatial, attenuation — 3D-расчёт | Spatial.hpp |
 * | AudioSystem::AudioEngine, EngineConfig, BusConfig, DuckRule — микшер | Engine.hpp |
 * | AudioSystem::SoundEvents, SoundEventDef — звуковые события из данных | SoundEvents.hpp |
 * | AudioSystem::AudioBackend, NullBackend, render_offline — вывод | Backend.hpp |
 */

#include <AudioSystem/Backend.hpp>
#include <AudioSystem/Clip.hpp>
#include <AudioSystem/Dsp.hpp>
#include <AudioSystem/Engine.hpp>
#include <AudioSystem/SoundEvents.hpp>
#include <AudioSystem/Spatial.hpp>
#include <AudioSystem/Types.hpp>
