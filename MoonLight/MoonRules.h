#pragma once
#include <cmath>
#include <algorithm>
struct MoonState {
    unsigned version = 1;
    float weight = 0, direction[3]{}, shadowStrength = .20f;
};
namespace moonrules {
inline float smooth(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3 - 2 * x);
}
inline MoonState state(float milliseconds, float shadowStrength = .20f) {
    MoonState s;
    s.shadowStrength = shadowStrength;
    if (!std::isfinite(milliseconds))
        return s;
    float hour = std::fmod(milliseconds / 3600000.f, 24.f);
    if (hour < 0)
        hour += 24;
    s.weight = hour >= 18 ? smooth((hour - 18) / 2) : hour <= 6 ? smooth((6 - hour) / 2) : 0;
    float angle = ((hour < 18 ? hour + 24 : hour) - 18) * (3.141592653589793f / 12);
    s.direction[0] = std::cos(angle) * .94f;
    s.direction[1] = std::cos(angle) * .341174f;
    s.direction[2] = std::sin(angle);
    float n = std::sqrt(s.direction[0] * s.direction[0] + s.direction[1] * s.direction[1] +
                        s.direction[2] * s.direction[2]);
    for (float &v : s.direction)
        v /= n;
    return s;
}
inline void lighting(float out[12], const float native[12], MoonState state, float amount) {
    std::copy(native, native + 12, out);
    float w = state.weight * amount;
    const float front[] = {.32f, .38f, .5f}, back[] = {.07f, .085f, .12f};
    for (int i = 0; i < 3; i++) {
        out[i] = std::clamp(out[i] + front[i] * w, 0.f, 1.f);
        out[4 + i] = std::clamp(out[4 + i] + back[i] * w, 0.f, 1.f);
        if (state.weight > 0)
            out[8 + i] = state.direction[i];
    }
    float n = std::sqrt(out[8] * out[8] + out[9] * out[9] + out[10] * out[10]);
    if (n > .001f)
        for (int i = 8; i < 11; i++)
            out[i] /= n;
}
inline void cloud(float out[4], const float native[4], float weight, float nightScale) {
    std::copy(native, native + 4, out);
    const float tint[] = {.88f, .94f, 1.f};
    for (int i = 0; i < 3; i++)
        out[i] *= 1 - weight + weight * nightScale * tint[i];
}
} // namespace moonrules

namespace moonrules {
struct ShadowCycle {
    float direction[3]{}, strength = 0;
};
inline ShadowCycle shadows(float milliseconds, float dayStrength, float nightStrength) {
    auto moon = state(milliseconds, nightStrength);
    ShadowCycle out;
    bool day = moon.direction[2] < 0;
    for (int i = 0; i < 3; i++)
        out.direction[i] = day ? -moon.direction[i] : moon.direction[i];
    float horizon = smooth(out.direction[2] / .5f);
    out.strength = day ? dayStrength * horizon : nightStrength * moon.weight * horizon;
    return out;
}
} // namespace moonrules
