#pragma once

// Pure-logic animation-clip sampling: no Vulkan, no GPU, no engine deps beyond
// glm. Deliberately does NOT include ModelAsset.h (which pulls in the RHI
// headers) so the headless anim-sampling-test can link it standalone, exactly
// like Lightkeeper's Level.h. ModelAsset.h includes THIS header for the clip
// types; World.cpp and the sampling test include it for the sample functions.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace candela {

// One animated TRS track targeting a single glTF node. Keyframe inputs live in
// `times` (seconds, sorted); outputs in `values` — vec3 in xyz for
// translation/scale, quaternion xyzw for rotation.
struct AnimationChannel {
    enum class Path : uint8_t { Translation, Rotation, Scale };
    enum class Interp : uint8_t { Step, Linear, CubicSpline };

    int targetNode = -1; // index into ModelAsset::nodes
    Path path = Path::Translation;
    Interp interp = Interp::Linear;
    std::vector<float> times;
    std::vector<glm::vec4> values;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f; // max keyframe time across channels
    std::vector<AnimationChannel> channels;
};

// Index of the last keyframe with times[i] <= t, clamped to [0, size-1].
inline size_t animKeyIndex(const std::vector<float>& times, float t) {
    if (times.size() <= 1) {
        return 0;
    }
    if (t <= times.front()) {
        return 0;
    }
    if (t >= times.back()) {
        return times.size() - 1;
    }
    size_t lo = 0;
    size_t hi = times.size() - 1;
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (times[mid] <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Normalized [0,1] blend factor between keyframe i and i+1 at time t.
inline float animBlendFactor(const AnimationChannel& channel, size_t i,
                             float t) {
    if (channel.interp == AnimationChannel::Interp::Step) {
        return 0.0f;
    }
    if (i + 1 >= channel.times.size()) {
        return 0.0f;
    }
    const float t0 = channel.times[i];
    const float t1 = channel.times[i + 1];
    const float dt = t1 - t0;
    if (dt <= 0.0f) {
        return 0.0f;
    }
    return glm::clamp((t - t0) / dt, 0.0f, 1.0f);
}

// Samples a translation/scale channel. CubicSpline collapses to linear on the
// spline vertices (in/out tangents are dropped at import — see ModelAsset.cpp).
inline glm::vec3 sampleVec3(const AnimationChannel& channel, float t) {
    if (channel.values.empty()) {
        return glm::vec3(0.0f);
    }
    const size_t i = animKeyIndex(channel.times, t);
    const glm::vec3 a = glm::vec3(channel.values[i]);
    if (i + 1 >= channel.values.size() ||
        channel.interp == AnimationChannel::Interp::Step) {
        return a;
    }
    const glm::vec3 b = glm::vec3(channel.values[i + 1]);
    return glm::mix(a, b, animBlendFactor(channel, i, t));
}

// Samples a rotation channel (values are quaternions in xyzw); slerps between
// keyframes. Result is normalized.
inline glm::quat sampleQuat(const AnimationChannel& channel, float t) {
    if (channel.values.empty()) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const size_t i = animKeyIndex(channel.times, t);
    const glm::vec4 va = channel.values[i];
    const glm::quat a(va.w, va.x, va.y, va.z); // glm::quat(w, x, y, z)
    if (i + 1 >= channel.values.size() ||
        channel.interp == AnimationChannel::Interp::Step) {
        return glm::normalize(a);
    }
    const glm::vec4 vb = channel.values[i + 1];
    const glm::quat b(vb.w, vb.x, vb.y, vb.z);
    return glm::normalize(glm::slerp(a, b, animBlendFactor(channel, i, t)));
}

} // namespace candela
