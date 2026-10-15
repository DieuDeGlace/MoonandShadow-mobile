// Made with the aid of AI (OpenAI ChatGPT/Codex).
//
// MoonandShadow.dll combines MoonLight (moonlit nights, visible moon) and
// SteamShadows (sun/moon shadows) into one mod sharing a single shadow map.
#define MOON_AND_SHADOW
#include "MoonLight/MoonLight.cpp"
#include "SteamShadows/SteamShadows.cpp"
class MoonandShadow : public GenericMod {
  public:
    void Initialize() override {
        moonlight::attach();
        shadows::attach();
    }
    // Finish anything not yet drawn this frame, then reset per-frame state.
    void OnPresent(IDXGISwapChain *, UINT, UINT) override {
        moonlight::renderSky();
        shadows::render();
        shadows::commitClouds();
        moonlight::clearSky();
        moonlight::skyFinished = false;
        shadows::clearDraws();
        shadows::frameOriginKnown = false;
        shadows::finished = false;
        shadows::presentTiming();
    }
};
static_assert(sizeof(GenericMod) == 0x428, "Loader API 7.3 required");
EXPORT GenericMod *MakeMod() {
    return new MoonandShadow();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}

// Called by TemporalAA/alpha-lookalike before their full-screen passes.
extern "C" __declspec(dllexport) void WINAPI FinishWorldLighting() {
    moonlight::renderSky();
    shadows::render();
}
