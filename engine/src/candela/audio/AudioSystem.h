#pragma once

namespace candela {

class World;
class AudioEngine;

// Per-frame bridge between the ECS and the AudioEngine. Follows the repo's
// "World owns no systems; the app drives them" model — construct one with an
// engine reference and call update() each frame after World::updateTransforms().
//
// It (1) sets the 3D listener from the first active AudioListener entity's
// WorldTransform, (2) starts autoplay AudioSources exactly once, and (3) keeps
// spatial sources' positions in sync with their WorldTransform.
class AudioSystem {
public:
    explicit AudioSystem(AudioEngine& engine);

    void update(World& world);

private:
    AudioEngine& m_engine;
};

} // namespace candela
