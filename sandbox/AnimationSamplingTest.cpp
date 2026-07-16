// Pure-logic animation-sampling self-test: no engine, no GPU, links only glm
// and the header-only AnimationSampling.h, so it runs on any build machine
// (like lightkeeper-leveltest). Exit code 0 = pass.
#include "candela/assets/AnimationSampling.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

bool approx(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) &&
           approx(a.z, b.z, eps);
}

candela::AnimationChannel makeVec3Channel(
    candela::AnimationChannel::Path path,
    candela::AnimationChannel::Interp interp, std::vector<float> times,
    std::vector<glm::vec3> values) {
    candela::AnimationChannel channel;
    channel.path = path;
    channel.interp = interp;
    channel.times = std::move(times);
    for (const glm::vec3& v : values) {
        channel.values.push_back(glm::vec4(v, 0.0f));
    }
    return channel;
}

} // namespace

int main() {
    using candela::AnimationChannel;

    // --- Translation, linear interpolation ---
    {
        const AnimationChannel t = makeVec3Channel(
            AnimationChannel::Path::Translation,
            AnimationChannel::Interp::Linear, {0.0f, 1.0f},
            {glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f)});
        CHECK(approx(candela::sampleVec3(t, 0.0f), glm::vec3(0.0f)));
        CHECK(approx(candela::sampleVec3(t, 1.0f), glm::vec3(2.0f, 0.0f, 0.0f)));
        CHECK(approx(candela::sampleVec3(t, 0.5f), glm::vec3(1.0f, 0.0f, 0.0f)));
        CHECK(approx(candela::sampleVec3(t, 0.25f),
                     glm::vec3(0.5f, 0.0f, 0.0f)));
        // Clamp before first / after last keyframe.
        CHECK(approx(candela::sampleVec3(t, -1.0f), glm::vec3(0.0f)));
        CHECK(approx(candela::sampleVec3(t, 5.0f), glm::vec3(2.0f, 0.0f, 0.0f)));
    }

    // --- Scale, step interpolation holds the left key ---
    {
        const AnimationChannel s = makeVec3Channel(
            AnimationChannel::Path::Scale, AnimationChannel::Interp::Step,
            {0.0f, 1.0f}, {glm::vec3(1.0f), glm::vec3(2.0f)});
        CHECK(approx(candela::sampleVec3(s, 0.0f), glm::vec3(1.0f)));
        CHECK(approx(candela::sampleVec3(s, 0.99f), glm::vec3(1.0f)));
        CHECK(approx(candela::sampleVec3(s, 1.0f), glm::vec3(2.0f)));
    }

    // --- Single keyframe: any time returns that key ---
    {
        const AnimationChannel one = makeVec3Channel(
            AnimationChannel::Path::Translation,
            AnimationChannel::Interp::Linear, {0.5f},
            {glm::vec3(5.0f, 6.0f, 7.0f)});
        CHECK(approx(candela::sampleVec3(one, 0.0f),
                     glm::vec3(5.0f, 6.0f, 7.0f)));
        CHECK(approx(candela::sampleVec3(one, 10.0f),
                     glm::vec3(5.0f, 6.0f, 7.0f)));
    }

    // --- Rotation, slerp: identity → 90° about Y, midpoint is 45° about Y ---
    {
        const float s45 = std::sqrt(0.5f); // sin(45°) = cos(45°)
        AnimationChannel r;
        r.path = AnimationChannel::Path::Rotation;
        r.interp = AnimationChannel::Interp::Linear;
        r.times = {0.0f, 1.0f};
        // Stored xyzw (glTF convention).
        r.values = {glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                    glm::vec4(0.0f, s45, 0.0f, s45)};

        const glm::quat q0 = candela::sampleQuat(r, 0.0f);
        CHECK(approx(q0.w, 1.0f) && approx(q0.x, 0.0f) && approx(q0.y, 0.0f) &&
              approx(q0.z, 0.0f));

        const glm::quat q1 = candela::sampleQuat(r, 1.0f);
        CHECK(approx(std::fabs(q1.w), s45) && approx(std::fabs(q1.y), s45));

        const glm::quat mid = candela::sampleQuat(r, 0.5f);
        const float cos22 = std::cos(glm::radians(22.5f));
        const float sin22 = std::sin(glm::radians(22.5f));
        // Compare up to sign (q and -q are the same rotation).
        CHECK(approx(std::fabs(mid.w), cos22));
        CHECK(approx(std::fabs(mid.y), sin22));
        CHECK(approx(mid.x, 0.0f) && approx(mid.z, 0.0f));
        // Result is a unit quaternion.
        CHECK(approx(glm::length(mid), 1.0f));
    }

    // --- Loop-wrap parity: fmod at duration+eps equals eps (mirrors
    //     World::updateAnimations' looping) ---
    {
        const AnimationChannel t = makeVec3Channel(
            AnimationChannel::Path::Translation,
            AnimationChannel::Interp::Linear, {0.0f, 2.0f},
            {glm::vec3(0.0f), glm::vec3(4.0f, 0.0f, 0.0f)});
        const float duration = 2.0f;
        const float eps = 0.3f;
        const float wrapped = std::fmod(duration + eps, duration);
        CHECK(approx(candela::sampleVec3(t, wrapped),
                     candela::sampleVec3(t, eps)));
    }

    if (g_failures == 0) {
        std::printf("anim-sampling-test: all checks passed\n");
        return 0;
    }
    std::printf("anim-sampling-test: %d FAILURES\n", g_failures);
    return 1;
}
