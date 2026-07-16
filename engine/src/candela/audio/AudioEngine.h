#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace candela {

// Parameters for a played instance. Spatial sources are positioned in world
// space and attenuated linearly between [minDistance, maxDistance]; non-spatial
// sources play as flat 2D at `volume`.
struct SoundParams {
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = false;
    glm::vec3 position{0.0f};
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
};

// Wraps a single miniaudio ma_engine. Loads/decodes .wav/.ogg/.mp3 through
// miniaudio's resource manager and plays one-shot, looping, and 3D-spatial
// sounds. All miniaudio types are hidden behind a PIMPL so this header pulls in
// only glm + std.
//
// Degrades gracefully: if no playback device can be opened, init() returns
// false, valid() stays false, and every call becomes a silent no-op so
// headless/CI runs keep going without audio hardware.
class AudioEngine {
public:
    using InstanceId = uint32_t;
    static constexpr InstanceId kInvalidInstance = 0;

    enum class Backend {
        Auto, // pick a real playback device; fail gracefully if none exists
        Null, // force miniaudio's null backend — hardware-free, for CI/tests
    };

    AudioEngine();
    ~AudioEngine(); // shutdown() if still initialized
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init(Backend backend = Backend::Auto);
    void shutdown(); // idempotent
    bool valid() const;

    // Master volume (linear gain, clamped to >= 0).
    void setMasterVolume(float linear);
    float masterVolume() const;

    // 3D listener (listener index 0). `forward`/`up` need not be normalized.
    void setListener(const glm::vec3& position, const glm::vec3& forward,
                     const glm::vec3& up);

    // Fire-and-forget 2D one-shot; lifetime owned by the engine, no handle.
    void playOneShot(const std::filesystem::path& clip, float volume = 1.0f);

    // Persistent, controllable instance (looping and/or entity-tracked spatial
    // sources). Returns kInvalidInstance on failure (invalid engine, missing
    // file). Fully decoded up front — these are short SFX, not music streams.
    InstanceId play(const std::filesystem::path& clip, const SoundParams& params);

    // Play a synthesized/in-memory clip: interleaved f32 PCM copied into an
    // engine-owned buffer, wrapped as a data source. Same control surface as
    // play(). Used by the headless --audiotest (no committed binary asset).
    InstanceId playMemory(const float* interleaved, uint64_t frameCount,
                          uint32_t channels, uint32_t sampleRate,
                          const SoundParams& params);

    void stop(InstanceId id);
    bool isPlaying(InstanceId id) const;
    void setInstancePosition(InstanceId id, const glm::vec3& position);
    void setInstanceVolume(InstanceId id, float volume);
    void setInstancePitch(InstanceId id, float pitch);

    // Reaps finished non-looping instances. Call once per frame.
    void update();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace candela
