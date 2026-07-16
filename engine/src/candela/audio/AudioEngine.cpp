#include "candela/audio/AudioEngine.h"

#include "candela/core/Compiler.h"
#include "candela/core/Log.h"

CD_PUSH_DISABLE_WARNINGS
#include <miniaudio.h> // declarations only; the impl lives in MiniaudioImpl.cpp
CD_POP_WARNINGS

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace candela {

struct AudioEngine::Impl {
    bool initialized = false;
    bool ownsContext = false;
    ma_context context{}; // only initialized for Backend::Null
    ma_engine engine{};

    struct Instance {
        ma_sound sound{};
        bool soundInitialized = false;           // guards ma_sound_uninit
        std::vector<float> pcm;                  // owned PCM for memory clips
        std::unique_ptr<ma_audio_buffer> buffer; // data source over `pcm`

        // ma_sound must be uninitialized before its backing buffer/pcm die, and
        // only if it was actually initialized (uninit on a zeroed sound is
        // undefined). This ordering (sound, then buffer) is the reason
        // instances are destroyed via this destructor rather than ad hoc.
        ~Instance() {
            if (soundInitialized) {
                ma_sound_uninit(&sound);
            }
            if (buffer) {
                ma_audio_buffer_uninit(buffer.get());
            }
        }
    };

    // unique_ptr keeps the ma_sound address stable (miniaudio stores
    // self-pointers into its node graph, so the object must never move).
    std::unordered_map<InstanceId, std::unique_ptr<Instance>> instances;
    InstanceId nextId = 1;

    Instance* find(InstanceId id) {
        const auto it = instances.find(id);
        return it == instances.end() ? nullptr : it->second.get();
    }
};

namespace {

void applyParams(ma_sound& sound, const SoundParams& params) {
    ma_sound_set_volume(&sound, (std::max)(0.0f, params.volume));
    ma_sound_set_pitch(&sound, (std::max)(0.0001f, params.pitch));
    ma_sound_set_looping(&sound, params.loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(&sound,
                                        params.spatial ? MA_TRUE : MA_FALSE);
    if (params.spatial) {
        ma_sound_set_attenuation_model(&sound, ma_attenuation_model_linear);
        ma_sound_set_min_distance(&sound, params.minDistance);
        ma_sound_set_max_distance(&sound, params.maxDistance);
        ma_sound_set_position(&sound, params.position.x, params.position.y,
                              params.position.z);
    }
}

} // namespace

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(Backend backend) {
    if (m_impl->initialized) {
        return true;
    }

    ma_engine_config config = ma_engine_config_init();

    if (backend == Backend::Null) {
        // Force the null backend so tests need no hardware and always succeed.
        // The null device is timer-driven, so playback still advances in real
        // time (a genuine N-ms sleep progresses the sound).
        const ma_backend backends[] = {ma_backend_null};
        const ma_result cr =
            ma_context_init(backends, 1, nullptr, &m_impl->context);
        if (cr != MA_SUCCESS) {
            CD_WARN("Audio: null context init failed ({})",
                    ma_result_description(cr));
            return false;
        }
        m_impl->ownsContext = true;
        config.pContext = &m_impl->context;
    }

    const ma_result r = ma_engine_init(&config, &m_impl->engine);
    if (r != MA_SUCCESS) {
        CD_WARN("Audio: no playback device ({}); running silently",
                ma_result_description(r));
        if (m_impl->ownsContext) {
            ma_context_uninit(&m_impl->context);
            m_impl->ownsContext = false;
        }
        return false;
    }

    m_impl->initialized = true;
    CD_INFO("Audio: engine initialized ({} backend)",
            backend == Backend::Null ? "null" : "auto");
    return true;
}

void AudioEngine::shutdown() {
    if (!m_impl->initialized) {
        // A null-backend context may have been created even if engine init
        // failed; clean it up defensively.
        if (m_impl->ownsContext) {
            ma_context_uninit(&m_impl->context);
            m_impl->ownsContext = false;
        }
        return;
    }

    // Instances hold ma_sound objects that reference the engine, so tear them
    // down first (~Instance uninits the sound + its buffer).
    m_impl->instances.clear();

    ma_engine_uninit(&m_impl->engine);
    if (m_impl->ownsContext) {
        ma_context_uninit(&m_impl->context);
        m_impl->ownsContext = false;
    }
    m_impl->initialized = false;
}

bool AudioEngine::valid() const { return m_impl->initialized; }

void AudioEngine::setMasterVolume(float linear) {
    if (!m_impl->initialized) {
        return;
    }
    ma_engine_set_volume(&m_impl->engine, (std::max)(0.0f, linear));
}

float AudioEngine::masterVolume() const {
    if (!m_impl->initialized) {
        return 0.0f;
    }
    return ma_engine_get_volume(&m_impl->engine);
}

void AudioEngine::setListener(const glm::vec3& position,
                              const glm::vec3& forward, const glm::vec3& up) {
    if (!m_impl->initialized) {
        return;
    }
    const glm::vec3 dir = glm::length(forward) > 0.0f
                              ? glm::normalize(forward)
                              : glm::vec3(0.0f, 0.0f, -1.0f);
    ma_engine_listener_set_position(&m_impl->engine, 0, position.x, position.y,
                                    position.z);
    ma_engine_listener_set_direction(&m_impl->engine, 0, dir.x, dir.y, dir.z);
    ma_engine_listener_set_world_up(&m_impl->engine, 0, up.x, up.y, up.z);
}

void AudioEngine::playOneShot(const std::filesystem::path& clip, float volume) {
    if (!m_impl->initialized) {
        return;
    }
    // Fire-and-forget: miniaudio owns the sound and reaps it at the end.
    const ma_result r = ma_engine_play_sound(
        &m_impl->engine, clip.string().c_str(), nullptr);
    if (r != MA_SUCCESS) {
        CD_WARN("Audio: failed to play one-shot '{}' ({})", clip.string(),
                ma_result_description(r));
        return;
    }
    // ma_engine_play_sound is fire-and-forget and exposes no per-instance
    // handle, so per-shot volume can't be set here; callers needing volume
    // control should use play(). The parameter is kept for API symmetry.
    (void)volume;
}

AudioEngine::InstanceId AudioEngine::play(const std::filesystem::path& clip,
                                          const SoundParams& params) {
    if (!m_impl->initialized) {
        return kInvalidInstance;
    }

    auto inst = std::make_unique<Impl::Instance>();
    const ma_uint32 flags =
        MA_SOUND_FLAG_DECODE |
        (params.spatial ? 0u
                        : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION));
    const ma_result r =
        ma_sound_init_from_file(&m_impl->engine, clip.string().c_str(), flags,
                                nullptr, nullptr, &inst->sound);
    if (r != MA_SUCCESS) {
        CD_WARN("Audio: failed to load '{}' ({})", clip.string(),
                ma_result_description(r));
        return kInvalidInstance;
    }
    inst->soundInitialized = true;

    applyParams(inst->sound, params);
    ma_sound_start(&inst->sound);

    const InstanceId id = m_impl->nextId++;
    m_impl->instances.emplace(id, std::move(inst));
    return id;
}

AudioEngine::InstanceId AudioEngine::playMemory(const float* interleaved,
                                                uint64_t frameCount,
                                                uint32_t channels,
                                                uint32_t sampleRate,
                                                const SoundParams& params) {
    if (!m_impl->initialized || interleaved == nullptr || frameCount == 0 ||
        channels == 0) {
        return kInvalidInstance;
    }

    auto inst = std::make_unique<Impl::Instance>();
    inst->pcm.assign(interleaved,
                     interleaved + frameCount * static_cast<uint64_t>(channels));
    inst->buffer = std::make_unique<ma_audio_buffer>();

    ma_audio_buffer_config bufCfg = ma_audio_buffer_config_init(
        ma_format_f32, channels, frameCount, inst->pcm.data(), nullptr);
    bufCfg.sampleRate = sampleRate;
    // ma_audio_buffer_init references pData (does not copy); inst->pcm keeps the
    // samples alive for the buffer's lifetime.
    ma_result r = ma_audio_buffer_init(&bufCfg, inst->buffer.get());
    if (r != MA_SUCCESS) {
        CD_WARN("Audio: failed to init memory buffer ({})",
                ma_result_description(r));
        inst->buffer.reset();
        return kInvalidInstance;
    }

    const ma_uint32 flags =
        params.spatial ? 0u
                       : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);
    r = ma_sound_init_from_data_source(&m_impl->engine, inst->buffer.get(),
                                       flags, nullptr, &inst->sound);
    if (r != MA_SUCCESS) {
        CD_WARN("Audio: failed to init memory sound ({})",
                ma_result_description(r));
        ma_audio_buffer_uninit(inst->buffer.get());
        inst->buffer.reset();
        return kInvalidInstance;
    }
    inst->soundInitialized = true;

    applyParams(inst->sound, params);
    ma_sound_start(&inst->sound);

    const InstanceId id = m_impl->nextId++;
    m_impl->instances.emplace(id, std::move(inst));
    return id;
}

void AudioEngine::stop(InstanceId id) {
    if (!m_impl->initialized) {
        return;
    }
    const auto it = m_impl->instances.find(id);
    if (it == m_impl->instances.end()) {
        return;
    }
    ma_sound_stop(&it->second->sound);
    m_impl->instances.erase(it); // ~Instance uninits sound + buffer
}

bool AudioEngine::isPlaying(InstanceId id) const {
    if (!m_impl->initialized) {
        return false;
    }
    const Impl::Instance* inst = m_impl->find(id);
    return inst != nullptr && ma_sound_is_playing(&inst->sound) == MA_TRUE;
}

void AudioEngine::setInstancePosition(InstanceId id,
                                      const glm::vec3& position) {
    if (!m_impl->initialized) {
        return;
    }
    if (Impl::Instance* inst = m_impl->find(id)) {
        ma_sound_set_position(&inst->sound, position.x, position.y, position.z);
    }
}

void AudioEngine::setInstanceVolume(InstanceId id, float volume) {
    if (!m_impl->initialized) {
        return;
    }
    if (Impl::Instance* inst = m_impl->find(id)) {
        ma_sound_set_volume(&inst->sound, (std::max)(0.0f, volume));
    }
}

void AudioEngine::setInstancePitch(InstanceId id, float pitch) {
    if (!m_impl->initialized) {
        return;
    }
    if (Impl::Instance* inst = m_impl->find(id)) {
        ma_sound_set_pitch(&inst->sound, (std::max)(0.0001f, pitch));
    }
}

void AudioEngine::update() {
    if (!m_impl->initialized) {
        return;
    }
    // Reap finished, non-looping instances so the map doesn't grow across a
    // session of play() calls.
    for (auto it = m_impl->instances.begin();
         it != m_impl->instances.end();) {
        ma_sound& sound = it->second->sound;
        const bool looping = ma_sound_is_looping(&sound) == MA_TRUE;
        const bool ended = ma_sound_at_end(&sound) == MA_TRUE;
        if (!looping && ended) {
            it = m_impl->instances.erase(it); // ~Instance uninits sound + buffer
        } else {
            ++it;
        }
    }
}

} // namespace candela
