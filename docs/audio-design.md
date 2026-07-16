# Candela Audio Pillar — Implementation Design

Vertical slice: a miniaudio-backed `AudioEngine`, `AudioListener` / `AudioSource`
ECS components, an `AudioSystem` that syncs listener + spatial sources from the
transform hierarchy each frame, scene (de)serialization for both components, and
a hardware-free `--audiotest` headless proof. Everything degrades gracefully when
no audio device exists so CI/headless keeps running silently.

All conventions below were verified against the real repo (CMake FetchContent
patterns in `cmake/Dependencies.cmake`, the single-header impl-TU pattern in
`engine/src/candela/rhi/VmaUsage.cpp` + `assets/StbUsage.cpp`, the additive ECS
component + serializer pattern, and the `sandbox/main.cpp` JobSystem lifecycle).

---

## 1. Files

### Create
| File | Purpose |
|---|---|
| `engine/src/candela/audio/AudioEngine.h` | Public engine API (PIMPL, no miniaudio in header). |
| `engine/src/candela/audio/AudioEngine.cpp` | Implementation over `ma_engine` (declarations only). |
| `engine/src/candela/audio/MiniaudioImpl.cpp` | The ONE TU defining `MINIAUDIO_IMPLEMENTATION`. |
| `engine/src/candela/audio/AudioSystem.h` | `AudioSystem` — per-frame ECS→engine sync. |
| `engine/src/candela/audio/AudioSystem.cpp` | System implementation. |

### Modify (Read-first; write COMPLETE file to staging; `action:"modify"` in MANIFEST)
| File | Change (additive, localized) |
|---|---|
| `cmake/Dependencies.cmake` | Declare `miniaudio` FetchContent; add `miniaudio` INTERFACE target. |
| `engine/CMakeLists.txt` | Add 3 audio sources; per-file warning suppression for `MiniaudioImpl.cpp`; link `miniaudio`. |
| `engine/src/candela/scene/Components.h` | Append `AudioListener` + `AudioSource` structs. |
| `engine/src/candela/scene/SceneSerializer.cpp` | Append save/load blocks for both components. |
| `sandbox/main.cpp` | Add `--audiotest` early-exit path; wire `AudioEngine`+`AudioSystem` into the main loop. |

No shared file is reorganized; every edit is an appended block so the later
3-way branch merge stays clean.

---

## 2. CMake / FetchContent

miniaudio is a single header (`miniaudio.h` at repo root) that also ships a
`CMakeLists.txt` building extras/tests we don't want. Reuse the exact pattern the
repo already uses for ImGuizmo/stb: fetch sources only (bogus `SOURCE_SUBDIR`
suppresses `add_subdirectory`), then expose an `INTERFACE` include target.

**`cmake/Dependencies.cmake`** — add a `FetchContent_Declare` next to the others:

```cmake
# miniaudio is header-only; a bogus SOURCE_SUBDIR keeps FetchContent from
# add_subdirectory-ing its bundled CMake (which builds tests/extras).
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.21
  GIT_SHALLOW ON
  SOURCE_SUBDIR does-not-exist)
```

Add `miniaudio` to the existing `FetchContent_MakeAvailable(...)` list. After
that call, register the interface target (mirrors the `stb` INTERFACE block):

```cmake
# miniaudio has no usable CMake for us — expose the header as an interface
# target. On Linux the implementation TU needs pthread/dl/m; Windows/macOS
# link their audio backends via the OS SDK automatically.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE ${miniaudio_SOURCE_DIR})
if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(miniaudio INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
```

**`engine/CMakeLists.txt`**:
- Add to `add_library(candela STATIC ...)`:
  ```
  src/candela/audio/AudioEngine.cpp
  src/candela/audio/AudioSystem.cpp
  src/candela/audio/MiniaudioImpl.cpp
  ```
- Add `miniaudio` to `target_link_libraries(candela PUBLIC ...)`.
- Suppress warnings on the implementation TU (miniaudio.h under `/W4 /WX` /
  `-Wall -Werror` is far too noisy). The impl file also uses
  `CD_PUSH_DISABLE_WARNINGS` internally (covers MSVC `warning(push,0)`); add the
  GCC spelling to the existing per-file block alongside `VmaUsage.cpp`:
  ```cmake
  else()
    set_source_files_properties(
        src/candela/rhi/VmaUsage.cpp
        src/candela/assets/StbUsage.cpp
        src/candela/audio/MiniaudioImpl.cpp        # <-- added
        PROPERTIES COMPILE_OPTIONS "-w")
  endif()
  ```
  `AudioEngine.cpp` keeps warnings ON (it's our code); it includes `miniaudio.h`
  declarations only, wrapped in `CD_PUSH_DISABLE_WARNINGS/CD_POP_WARNINGS`. If a
  GCC declaration-only warning ever escapes, the fallback is to add
  `AudioEngine.cpp` to the same `-w` list — noted, not needed for the slice.

---

## 3. `MiniaudioImpl.cpp` (the implementation TU)

Mirrors `VmaUsage.cpp` exactly. Global `NOMINMAX`/`WIN32_LEAN_AND_MEAN` are
already set for MSVC by the root `CMakeLists.txt`.

```cpp
// Single translation unit hosting the miniaudio implementation. Every other
// TU includes <miniaudio.h> for declarations only.
#include "candela/core/Compiler.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING          // playback only — no need for the WAV/etc. writers
CD_PUSH_DISABLE_WARNINGS
#include <miniaudio.h>
CD_POP_WARNINGS
```

(Decoders for wav/mp3/flac are on by default; vorbis/.ogg is available via
miniaudio's built-in stb_vorbis. We keep all decoders enabled — only encoding is
disabled — so `.wav/.ogg/.mp3` load as required.)

---

## 4. `AudioEngine` — `engine/src/candela/audio/AudioEngine.{h,cpp}`

PIMPL so miniaudio never appears in a public header. `ma_engine`/`ma_sound` are
by-value structs holding internal self-pointers, so instances are heap-allocated
and never moved after init.

### Header (`AudioEngine.h`)

```cpp
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace candela {

// Parameters for a played instance. Spatial sources are positioned in world
// space and attenuated between [minDistance, maxDistance]; non-spatial sources
// play as flat 2D at `volume`.
struct SoundParams {
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = false;
    glm::vec3 position{0.0f};
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
};

// Wraps a single ma_engine. Loads/decodes .wav/.ogg/.mp3 via miniaudio's
// resource manager (path-keyed decode cache is the RM's, by construction).
// Degrades gracefully: if no device can be opened, init() returns false, the
// engine stays invalid(), and every call is a silent no-op so headless/CI runs.
class AudioEngine {
public:
    using InstanceId = uint32_t;
    static constexpr InstanceId kInvalidInstance = 0;

    enum class Backend {
        Auto, // pick a real playback device; graceful failure if none
        Null, // force miniaudio's null backend — hardware-free, for CI/tests
    };

    AudioEngine();
    ~AudioEngine();                      // shutdown() if still initialized
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init(Backend backend = Backend::Auto);
    void shutdown();
    bool valid() const;                  // true only after a successful init

    // Master.
    void setMasterVolume(float linear);  // clamped >= 0
    float masterVolume() const;

    // 3D listener (listener index 0). `forward`/`up` need not be normalized.
    void setListener(const glm::vec3& position, const glm::vec3& forward,
                     const glm::vec3& up);

    // Fire-and-forget 2D one-shot; lifetime owned by the engine. No handle.
    void playOneShot(const std::filesystem::path& clip, float volume = 1.0f);

    // Persistent, controllable instance (looping and/or entity-tracked spatial
    // sources). Returns kInvalidInstance on failure (invalid engine, missing
    // file). Decoded up front (MA_SOUND_FLAG_DECODE) — these are short SFX.
    InstanceId play(const std::filesystem::path& clip, const SoundParams& params);

    // Headless/synthesized clip: interleaved f32 PCM copied into an owned
    // buffer, wrapped as a data source. Same control surface as play().
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
```

### Implementation notes (`AudioEngine.cpp`)

`Impl` holds:
```cpp
struct AudioEngine::Impl {
    bool initialized = false;
    bool ownsContext = false;
    ma_context context{};   // only initialized for Backend::Null
    ma_engine engine{};

    struct Instance {
        ma_sound sound{};
        std::vector<float> pcm;                 // owned PCM for memory clips
        std::unique_ptr<ma_audio_buffer> buffer; // data source over pcm
    };
    // unique_ptr so the ma_sound address is stable (miniaudio stores self-ptrs).
    std::unordered_map<InstanceId, std::unique_ptr<Instance>> instances;
    InstanceId nextId = 1;
};
```

- **`init(Auto)`**: `ma_engine_config cfg = ma_engine_config_init();` then
  `ma_result r = ma_engine_init(&cfg, &engine);`. On `r != MA_SUCCESS`:
  `CD_WARN("Audio: no playback device ({}); running silently", ma_result_description(r));`
  leave `initialized=false`, return `false`. The app keeps running.
- **`init(Null)`**: force the null backend so tests need no hardware and always
  succeed:
  ```cpp
  ma_backend backends[] = { ma_backend_null };
  ma_context_init(backends, 1, nullptr, &context);   ownsContext = true;
  ma_engine_config cfg = ma_engine_config_init();
  cfg.pContext = &context;
  ma_engine_init(&cfg, &engine);
  ```
  The null backend runs a timer-driven silent device, so playback advances in
  real time (a real N-ms sleep genuinely progresses the sound).
- **`shutdown()`**: `ma_sound_uninit` every instance (before the buffers/pcm die),
  clear map, `ma_engine_uninit(&engine)`, then `ma_context_uninit(&context)` if
  `ownsContext`. Idempotent; dtor calls it.
- Every public method early-returns (no-op / `kInvalidInstance` / `false`) when
  `!initialized`.
- **`setMasterVolume`**: `ma_engine_set_volume(&engine, max(0,linear))`.
  `masterVolume`: `ma_engine_get_volume`.
- **`setListener`**: `ma_engine_listener_set_position/-_direction(&engine, 0,...)`
  (direction = normalized forward), `..._set_world_up(&engine, 0, up...)`.
- **`play(path, params)`**:
  ```cpp
  auto inst = std::make_unique<Impl::Instance>();
  ma_uint32 flags = MA_SOUND_FLAG_DECODE |
      (params.spatial ? 0u : MA_SOUND_FLAG_NO_SPATIALIZATION);
  if (ma_sound_init_from_file(&engine, path.string().c_str(), flags,
                              nullptr, nullptr, &inst->sound) != MA_SUCCESS) {
      CD_WARN("Audio: failed to load '{}'", path.string());
      return kInvalidInstance;
  }
  applyParams(inst->sound, params);   // volume, pitch, loop, spatial pos+attenuation
  ma_sound_start(&inst->sound);
  InstanceId id = nextId++;
  instances.emplace(id, std::move(inst));
  return id;
  ```
- **`playMemory(...)`**: copy PCM into `inst->pcm`; build a
  `ma_audio_buffer_config` (`ma_format_f32`, channels, frameCount, `pcm.data()`);
  `ma_audio_buffer_init(&cfg, buffer.get())`;
  `ma_sound_init_from_data_source(&engine, buffer.get(), flags, nullptr, &inst->sound)`;
  same `applyParams` + `ma_sound_start`. (The data-source sample rate is taken
  from the buffer; `sampleRate` is stored on the config so pitch stays correct.)
- **`applyParams` helper** (file-local): `ma_sound_set_volume`,
  `ma_sound_set_pitch`, `ma_sound_set_looping(loop)`,
  `ma_sound_set_spatialization_enabled(spatial)`, and when spatial:
  `ma_sound_set_position`, `ma_sound_set_attenuation_model(ma_attenuation_model_linear)`,
  `ma_sound_set_min_distance`, `ma_sound_set_max_distance`.
- **`stop/isPlaying/setInstance*`**: look up by id; `stop` → `ma_sound_stop`
  then uninit + erase; `isPlaying` → `ma_sound_is_playing`;
  position/volume/pitch → the matching `ma_sound_set_*`.
- **`update()`**: erase instances whose `ma_sound_is_playing == MA_FALSE` and
  `ma_sound_at_end == MA_TRUE` and not looping (uninit first). Keeps the map from
  growing across a session of one-shot `play()` calls.

Include block:
```cpp
#include "candela/audio/AudioEngine.h"
#include "candela/core/Log.h"
CD_PUSH_DISABLE_WARNINGS
#include <miniaudio.h>       // declarations only; impl lives in MiniaudioImpl.cpp
CD_POP_WARNINGS
#include <algorithm>
#include <unordered_map>
#include <vector>
```

---

## 5. ECS components — append to `scene/Components.h`

```cpp
// Marks the entity whose WorldTransform drives the 3D audio listener (usually
// the camera/player). If several exist, the AudioSystem uses the first.
struct AudioListener {
    bool active = true;
};

// A sound emitter. `clip` is a filesystem path (AssetGuid routing is a
// follow-up). `instance`/`started` are runtime-only and are NOT serialized.
struct AudioSource {
    std::string clip;          // path to a .wav/.ogg/.mp3
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = true;       // false = flat 2D
    bool autoplay = true;      // start once on first AudioSystem tick
    float minDistance = 1.0f;
    float maxDistance = 100.0f;

    // Runtime state (not serialized).
    uint32_t instance = 0;     // AudioEngine::InstanceId; 0 = not started
    bool started = false;
};
```

(`#include <cstdint>` is already transitively available via existing headers;
add it explicitly to be safe. `std::string` is already included.)

---

## 6. `AudioSystem` — `engine/src/candela/audio/AudioSystem.{h,cpp}`

Follows the repo's "World owns no systems; the app calls them" model. Holds a
reference to the engine; called each frame **after** `World::updateTransforms()`.

### Header
```cpp
#pragma once
namespace candela {
class World;
class AudioEngine;

// Per-frame bridge: syncs the audio listener and spatial sources from the ECS
// transform hierarchy, and starts autoplay sources exactly once.
class AudioSystem {
public:
    explicit AudioSystem(AudioEngine& engine);
    void update(World& world);   // call after World::updateTransforms()
private:
    AudioEngine& m_engine;
};
} // namespace candela
```

### Implementation
```cpp
void AudioSystem::update(World& world) {
    if (!m_engine.valid()) return;
    auto& reg = world.registry;

    // 1) Listener from the first active AudioListener entity's WorldTransform.
    for (auto e : reg.view<AudioListener, WorldTransform>()) {
        if (!reg.get<AudioListener>(e).active) continue;
        const glm::mat4& m = reg.get<WorldTransform>(e).value;
        const glm::vec3 pos = glm::vec3(m[3]);
        const glm::vec3 forward = -glm::vec3(m[2]); // -Z is forward (glm cols)
        const glm::vec3 up = glm::vec3(m[1]);
        m_engine.setListener(pos, forward, up);
        break;
    }

    // 2) Sources: start autoplay once; keep spatial positions in sync.
    for (auto e : reg.view<AudioSource, WorldTransform>()) {
        auto& src = reg.get<AudioSource>(e);
        const glm::vec3 pos = glm::vec3(reg.get<WorldTransform>(e).value[3]);
        if (!src.started && src.autoplay) {
            SoundParams p{src.volume, src.pitch, src.loop, src.spatial,
                          pos, src.minDistance, src.maxDistance};
            src.instance = m_engine.play(src.clip, p);
            src.started = true;
        } else if (src.started && src.spatial &&
                   src.instance != AudioEngine::kInvalidInstance &&
                   m_engine.isPlaying(src.instance)) {
            m_engine.setInstancePosition(src.instance, pos);
        }
    }
}
```

Forward/up are read from the WorldTransform's rotation columns (glm is
column-major: `m[1]`=+Y/up, `m[2]`=+Z, forward = `-m[2]`). This matches the
engine `Camera::forward()` convention (camera looks down -Z).

---

## 7. Scene serialization — append to `scene/SceneSerializer.cpp`

**Save** (inside `worldToJson`, after the `camera` block, same style):
```cpp
if (registry.try_get<AudioListener>(entity)) {
    e["audioListener"] = registry.get<AudioListener>(entity).active;
}
if (const auto* s = registry.try_get<AudioSource>(entity)) {
    e["audioSource"] = {{"clip", s->clip}, {"volume", s->volume},
                        {"pitch", s->pitch}, {"loop", s->loop},
                        {"spatial", s->spatial}, {"autoplay", s->autoplay},
                        {"minDistance", s->minDistance},
                        {"maxDistance", s->maxDistance}};
}
```

**Load** (inside `worldFromJson`, after the `camera` block):
```cpp
if (e.contains("audioListener")) {
    world.registry.emplace<AudioListener>(entity,
        e["audioListener"].get<bool>());
}
if (e.contains("audioSource")) {
    const auto& j = e["audioSource"];
    AudioSource s;
    s.clip = j.value("clip", std::string{});
    s.volume = j.value("volume", 1.0f);
    s.pitch = j.value("pitch", 1.0f);
    s.loop = j.value("loop", false);
    s.spatial = j.value("spatial", true);
    s.autoplay = j.value("autoplay", true);
    s.minDistance = j.value("minDistance", 1.0f);
    s.maxDistance = j.value("maxDistance", 100.0f);
    world.registry.emplace<AudioSource>(entity, s);
}
```
Runtime fields (`instance`, `started`) are intentionally omitted → round-trip
stable. Uses `.value(key, default)` so older scenes and partial JSON load fine.

---

## 8. `sandbox/main.cpp` — headless test + loop wiring

### `--audiotest` (hardware-free proof)

Parse a `bool audioTest` flag in the existing arg loop
(`--audiotest` → `audioTest = true`). Handle it **before** `JobSystem::init()`
so the "early return after JobSystem::init must call shutdown" rule cannot be
tripped — the test needs neither the job system, a Window, nor a GPU:

```cpp
if (audioTest) {
    candela::AudioEngine engine;
    if (!engine.init(candela::AudioEngine::Backend::Null)) {
        CD_ERROR("Audio test: null backend init failed");
        return 1;
    }
    // Synthesize a 440 Hz sine: 0.5 s, mono, 48 kHz, interleaved f32.
    constexpr uint32_t kRate = 48000, kChannels = 1;
    constexpr uint64_t kFrames = kRate / 2;
    std::vector<float> pcm(kFrames);
    for (uint64_t i = 0; i < kFrames; ++i)
        pcm[i] = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f *
                                  static_cast<float>(i) / kRate);

    candela::SoundParams p; p.volume = 0.8f; // 2D, non-loop
    const auto id = engine.playMemory(pcm.data(), kFrames, kChannels, kRate, p);
    if (id == candela::AudioEngine::kInvalidInstance || !engine.isPlaying(id)) {
        CD_ERROR("Audio test: clip failed to start");
        engine.shutdown();
        return 1;
    }
    // Let the null-backend device advance ~200 ms.
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        engine.update();
    }
    engine.shutdown();
    CD_INFO("Audio headless test: PASS");
    return 0;
}
```
Add `#include <candela/audio/AudioEngine.h>`, `#include <cmath>`,
`#include <thread>` (chrono/vector already present). Reports PASS/FAIL and a
process exit code, so CI asserts on it without hardware.

### Main-loop wiring

Inside the existing inner scope (after `AssetRegistry assets{...}`), construct:
```cpp
candela::AudioEngine audio;
audio.init();                              // Auto backend; silent if no device
candela::AudioSystem audioSystem{audio};
```
Attach a listener entity synced to the fly camera (the sandbox camera is
engine-side, not an entity), created once before the loop:
```cpp
const entt::entity listener = world.createEntity("audio_listener");
world.registry.emplace<candela::AudioListener>(listener);
```
Each frame, right **after** `world.updateTransforms();` (and after `camera.update`):
```cpp
// Sync the listener entity to the fly camera, then run audio.
{
    auto& lt = world.registry.get<candela::LocalTransform>(listener);
    lt.translation = camera.position;
    lt.rotation = glm::quat(glm::vec3(camera.pitchRadians,
                                      camera.yawRadians, 0.0f));
}
world.updateTransforms();       // cheap; listener transform now current
audioSystem.update(world);
audio.update();
```
(Or, to avoid a second `updateTransforms`, set the listener LocalTransform
*before* the existing `updateTransforms()` call — preferred; shown separately for
clarity.) `AudioEngine`/`AudioSystem` live in the inner scope, so RAII shuts the
engine down before `JobSystem::shutdown()`.

No audio asset is committed, so no `AudioSource` is added to the default scene —
the system still runs (listener tracking) and any scene loaded with an
`audioSource` block whose `clip` file exists will play. This satisfies "wire into
a dedicated AudioSystem and call it from the loop" without committing binaries.

---

## 9. Headless test summary

- Target: `sandbox` executable, `--audiotest` flag (no GPU, no device, no job
  system). Exit 0 = PASS, non-zero = FAIL; logs `Audio headless test: PASS`.
- Forces miniaudio's **null backend** (`Backend::Null`) so it runs on any CI
  runner. Synthesizes a 440 Hz sine in-code (no committed binary asset),
  decodes/plays it through the real `AudioEngine` path (`playMemory` →
  `ma_sound` over a `ma_audio_buffer`), advances the device ~200 ms, verifies the
  instance started and shut down cleanly.
- Exercises: engine init, in-memory clip load, instance playback + `isPlaying`,
  `update()` reaping, and clean `shutdown()`.

---

## 10. Cross-compiler / correctness checklist

- MSVC + GCC: miniaudio impl TU gets `warning(push,0)` (MSVC) and `-w` (GCC);
  our audio `.cpp`s keep `/W4 /WX` / `-Wall -Werror` on and pass.
- No miniaudio types in any public header (PIMPL) → no include leakage, header
  stays cheap; `AudioEngine.h` pulls only glm + std.
- Graceful degradation: `init(Auto)` failure → `valid()==false` → every call a
  no-op; sandbox + all headless modes keep running silently.
- `ma_sound`/`ma_engine` never moved after init (heap `unique_ptr<Instance>`,
  members-by-value `Impl`).
- Shared-file edits (`Components.h`, `SceneSerializer.cpp`, `sandbox/main.cpp`,
  `engine/CMakeLists.txt`, `cmake/Dependencies.cmake`) are additive blocks only.
- Vertex layout, GBufferPush, RenderGraph, TLAS, fastgltf — untouched. Audio is
  orthogonal to the render path.

## 11. Follow-ups (out of slice)
- Route `AudioSource.clip` through `AssetRegistry`/`AssetGuid` instead of raw
  paths (registry currently models only `ModelAsset`).
- Streaming (`MA_SOUND_FLAG_STREAM`) for long music beds vs. decoded SFX.
- Editor inspector UI for `AudioSource`/`AudioListener`.
- Doppler/velocity on spatial sources (needs per-entity velocity).
