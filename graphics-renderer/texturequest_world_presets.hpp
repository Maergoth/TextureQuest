#pragma once
#include <algorithm>

namespace tq_world {
inline int ShadowPresetLimit(int preset, int supported) {
    const int limit = preset == 0 ? 10 : preset == 1 ? 50 : supported;
    return std::clamp(limit, 1, supported);
}
inline int ShadowPresetIndex(int limit, int supported) {
    for (int i = 0; i < 3; ++i)
        if (limit == ShadowPresetLimit(i, supported)) return i;
    return -1;
}
inline int ShadowQualityIndex(int lights, int particles, int supported, int supportedParticles) {
    for (int i = 0; i < 3; ++i)
        if (lights == ShadowPresetLimit(i, supported)
            && particles == ShadowPresetLimit(i, supportedParticles)) return i;
    return -1;
}
inline int LightPresetLimit(int preset, int supported) {
    return std::clamp(preset == 0 ? 16 : preset == 1 ? 64 : supported, 1, supported);
}
inline int LightQualityIndex(int lights, int particles, int supported, int supportedParticles) {
    for (int i = 0; i < 3; ++i)
        if (lights == LightPresetLimit(i, supported)
            && particles == LightPresetLimit(i, supportedParticles)) return i;
    return -1;
}
struct PresetValues {
    float ShadowStrength, ShadowSoftness, AORadius, AOStrength;
    int ShadowedLights, Anisotropy;
};
template<class Read, class Write>
bool MigratePresets(Read read, Write write, PresetValues& values) {
    int version = 0;
    if (read("TextureQuestPresetVersion", version) && version >= 2) return false;
    // All backups precede the first setting change. A retry keeps the first
    // saved numeric values, even after an interrupted defaults write.
    const auto backup = [&](const char* key, auto value) {
        auto previous = value;
        if (!read(key, previous)) write(key, value);
    };
    if (version == 1) {
        // Keep the first pre-v2 value through a partial write/retry. Custom
        // softness and every v1 backup remain unchanged.
        backup("TextureQuestPreset2PreviousShadowSoftness", values.ShadowSoftness);
        if (values.ShadowSoftness == 0.05f) {
            values.ShadowSoftness = 0.15f;
            write("ShadowSoftness", values.ShadowSoftness);
        }
        write("TextureQuestPresetVersion", 2);
        return true;
    }
    backup("TextureQuestPreset1PreviousShadowStrength", values.ShadowStrength);
    backup("TextureQuestPreset1PreviousShadowSoftness", values.ShadowSoftness);
    backup("TextureQuestPreset1PreviousOcclusionRadius", values.AORadius);
    backup("TextureQuestPreset1PreviousOcclusionStrength", values.AOStrength);
    backup("TextureQuestPreset1PreviousShadowedLights", values.ShadowedLights);
    backup("TextureQuestPreset1PreviousAnisotropy", values.Anisotropy);
    values = {1.0f, 0.15f, 0.35f, 1.25f, 50, 16};
    write("ShadowStrength", values.ShadowStrength);
    write("ShadowSoftness", values.ShadowSoftness);
    write("OcclusionRadius", values.AORadius);
    write("OcclusionStrength", values.AOStrength);
    write("ShadowedLights", values.ShadowedLights);
    write("Anisotropy", values.Anisotropy);
    write("TextureQuestPresetVersion", 2);
    return true;
}
} // namespace tq_world
