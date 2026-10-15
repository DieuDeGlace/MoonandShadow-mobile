#include "MoonRules.h"
#include <cassert>
#include <cstdio>
int main() {
    for (float hour : {8.f, 12.f, 16.f, 18.f, 6.f})
        assert(moonrules::state(hour * 3600000).weight == 0);
    for (float hour : {0.f, 2.f, 4.f, 20.f, 22.f})
        assert(moonrules::state(hour * 3600000).weight == 1);
    assert(moonrules::state(19 * 3600000.f).weight == .5f);
    assert(moonrules::state(5 * 3600000.f).weight == .5f);
    for (int minute = 0; minute < 1440; minute++) {
        auto a = moonrules::state(minute * 60000.f), b = moonrules::state((minute + 1) * 60000.f);
        assert(std::abs(a.weight - b.weight) < .013);
        float n = 0;
        for (float v : a.direction)
            n += v * v;
        assert(std::abs(n - 1) < 1e-5);
        if (a.weight > 0)
            assert(a.direction[2] >= 0);
    }
    float native[] = {.1, .1, .1, 1, .1, .1, .1, 1, .6, 0, -.8, 0}, out[12];
    auto night = moonrules::state(0);
    moonrules::lighting(out, native, night, .08f);
    for (int i = 0; i < 3; i++)
        assert(out[i] > native[i] && out[i] - native[i] <= .041f);
    assert(out[3] == native[3] && out[7] == native[7] && out[11] == native[11]);
    assert(out[10] > .99);
    moonrules::lighting(out, native, moonrules::state(12 * 3600000.f), .08f);
    for (int i = 0; i < 12; i++)
        assert(out[i] == native[i]);
    float cloud[] = {1, 1, 1, .7f}, tinted[4];
    moonrules::cloud(tinted, cloud, 1, .55f);
    assert(tinted[0] < .55 && tinted[2] == .55f && tinted[3] == cloud[3]);
    moonrules::cloud(tinted, cloud, 0, .55f);
    for (int i = 0; i < 4; i++)
        assert(tinted[i] == cloud[i]);
    auto midday = moonrules::shadows(12 * 3600000.f, .6f, .2f),
         midnight = moonrules::shadows(0, .6f, .2f);
    assert(std::abs(midday.strength - .6f) < 1e-5 && std::abs(midnight.strength - .2f) < 1e-5);
    assert(midday.direction[2] > .99 && midnight.direction[2] > .99);
    // Full strength through the afternoon; fading out over the last 1.5 hours.
    auto afternoonLate = moonrules::shadows(16.5f * 3600000.f, .6f, .2f),
         evening = moonrules::shadows(17.5f * 3600000.f, .6f, .2f),
         dusk = moonrules::shadows(18 * 3600000.f, .6f, .2f);
    assert(std::abs(afternoonLate.strength - midday.strength) < 1e-3);
    assert(evening.strength > 0 && evening.strength < midday.strength && dusk.strength < 1e-6);
    assert(moonrules::shadows(0, .6f, 0).strength == 0);
    // No shadows at all while the sun or moon is at the horizon (dusk and dawn).
    for (float hour : {17.75f, 17.9f, 18.f, 18.1f, 18.25f, 5.75f, 5.9f, 6.f, 6.1f, 6.25f})
        assert(moonrules::shadows(hour * 3600000.f, .6f, .2f).strength == 0);
    assert(moonrules::shadows(17.6f * 3600000.f, .6f, .2f).strength > 0);
    assert(moonrules::shadows(6.5f * 3600000.f, .6f, .2f).strength > 0);
    // The moonlit light direction turns gradually at moonrise and moonset, even
    // from a native direction opposite the rising moon.
    const float natives[][3] = {{0, 0, 1}, {-.94f, -.341f, 0}, {.6f, 0, -.8f}};
    for (auto &dir : natives) {
        float light[12] = {.5f, .5f, .5f, 1, .2f, .2f, .2f, 1, dir[0], dir[1], dir[2], 0};
        float previous[12];
        moonrules::lighting(previous, light, moonrules::state(0), .04f);
        for (int minute = 1; minute <= 1440; minute++) {
            float now[12];
            moonrules::lighting(now, light, moonrules::state(minute * 60000.f), .04f);
            float d = now[8] * previous[8] + now[9] * previous[9] + now[10] * previous[10];
            assert(d > std::cos(3 * 3.14159265f / 180)); // Under 3 degrees per game minute.
            std::copy(now, now + 12, previous);
        }
    }
    auto morning = moonrules::shadows(9 * 3600000.f, .6f, .2f),
         afternoon = moonrules::shadows(15 * 3600000.f, .6f, .2f);
    assert(morning.direction[0] * afternoon.direction[0] < 0);
    for (int minute = 1; minute < 1440; minute++) {
        auto a = moonrules::shadows((minute - 1) * 60000.f, .6f, .2f),
             b = moonrules::shadows(minute * 60000.f, .6f, .2f);
        assert(std::abs(a.strength - b.strength) < .02f); // No visible step per game minute.
    }
    puts(
        "Subtle brightness limits, unchanged daytime/alpha, normalized moon direction, smooth dawn/dusk and night-only cloud dimming: PASS");
}
