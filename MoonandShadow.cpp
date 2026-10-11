// Made with the aid of AI (OpenAI ChatGPT/Codex).
#define MOON_AND_SHADOW
#include "MoonLight/MoonLight.cpp"
#include "SteamShadows/SteamShadows.cpp"
class MoonandShadow : public GenericMod {
  public:
    void Initialize() override {
        moonlight::attach();
        shadows::attach();
    }
    void OnPresent(IDXGISwapChain *, UINT, UINT) override {
        moonlight::renderSky();
        shadows::render();
        shadows::commitClouds();
        moonlight::clearSky();
        moonlight::skyFinished = false;
        shadows::clearDraws();
        shadows::frameOriginKnown = false;
        shadows::finished = false;
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

extern "C" __declspec(dllexport) void WINAPI FinishWorldLighting() {
    moonlight::renderSky();
    shadows::render();
}
