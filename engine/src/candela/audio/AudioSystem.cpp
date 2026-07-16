#include "candela/audio/AudioSystem.h"

#include "candela/audio/AudioEngine.h"
#include "candela/scene/Components.h"
#include "candela/scene/World.h"

#include <glm/glm.hpp>

namespace candela {

AudioSystem::AudioSystem(AudioEngine& engine) : m_engine(engine) {}

void AudioSystem::update(World& world) {
    if (!m_engine.valid()) {
        return;
    }
    auto& registry = world.registry;

    // 1) Listener from the first active AudioListener entity's WorldTransform.
    //    glm is column-major: m[3] = translation, m[1] = +Y (up),
    //    forward = -m[2] (the camera looks down -Z; matches Camera::forward()).
    for (const entt::entity entity :
         registry.view<AudioListener, WorldTransform>()) {
        if (!registry.get<AudioListener>(entity).active) {
            continue;
        }
        const glm::mat4& m = registry.get<WorldTransform>(entity).value;
        const glm::vec3 position = glm::vec3(m[3]);
        const glm::vec3 forward = -glm::vec3(m[2]);
        const glm::vec3 up = glm::vec3(m[1]);
        m_engine.setListener(position, forward, up);
        break;
    }

    // 2) Sources: start autoplay once, then keep spatial positions in sync.
    for (const entt::entity entity :
         registry.view<AudioSource, WorldTransform>()) {
        auto& source = registry.get<AudioSource>(entity);
        const glm::vec3 position =
            glm::vec3(registry.get<WorldTransform>(entity).value[3]);

        if (!source.started && source.autoplay && !source.clip.empty()) {
            SoundParams params;
            params.volume = source.volume;
            params.pitch = source.pitch;
            params.loop = source.loop;
            params.spatial = source.spatial;
            params.position = position;
            params.minDistance = source.minDistance;
            params.maxDistance = source.maxDistance;
            source.instance = m_engine.play(source.clip, params);
            source.started = true;
        } else if (source.started && source.spatial &&
                   source.instance != AudioEngine::kInvalidInstance &&
                   m_engine.isPlaying(source.instance)) {
            m_engine.setInstancePosition(source.instance, position);
        }
    }
}

} // namespace candela
