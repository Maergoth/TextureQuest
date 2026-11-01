#pragma once
// TextureQuest.ini beside the add-on: UTF-8 without BOM, one [TextureQuest]
// section of Key=Value lines. ReShade.ini is never read or written.
#include <windows.h>
#include <fstream>
#include <map>
#include <string>

namespace tq {

struct Config {
    std::wstring Map, Downloads, Shortcut, GamePaks;
    bool Effects = false, DLSS = false;
    std::string DLSSBackend = "feeder";
};

inline std::wstring Widen(const std::string& text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), int(wide.size()));
    return wide;
}

inline bool ReadConfig(const std::wstring& ini, Config& config, std::string& error) {
    // A reused Config must never retain an earlier opt-in after an absent,
    // invalid or otherwise unsuccessful read. Commit only after all checks.
    config.Effects = false;
    config.DLSS = false;
    config.DLSSBackend = "feeder";
    error.clear();
    std::ifstream file(ini, std::ios::binary);
    if (!file) {
        error = "TextureQuest.ini not found next to the add-on";
        return false;
    }
    std::map<std::string, std::string> values;
    bool section = false, effects_seen = false, effects = false, dlss_seen = false, dlss = false;
    bool backend_seen = false;
    std::string backend = "feeder";
    for (std::string line; std::getline(file, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[') {
            section = line == "[TextureQuest]";
            continue;
        }
        const size_t equals = line.find('=');
        if (section && equals != std::string::npos) {
            if (line.substr(0, equals) == "Effects") {
                const std::string value = line.substr(equals + 1);
                if (effects_seen || (value != "0" && value != "1")) {
                    error = "TextureQuest.ini Effects must be one optional 0 or 1 entry";
                    return false;
                }
                effects_seen = true;
                effects = value == "1";
            }
            if (line.substr(0, equals) == "DLSS") {
                const std::string value = line.substr(equals + 1);
                if (dlss_seen || (value != "0" && value != "1")) {
                    error = "TextureQuest.ini DLSS must be one optional 0 or 1 entry";
                    return false;
                }
                dlss_seen = true;
                dlss = value == "1";
            }
            if (line.substr(0, equals) == "DLSSBackend") {
                const std::string value = line.substr(equals + 1);
                if (backend_seen || (value != "feeder" && value != "aio")) {
                    error = "TextureQuest.ini DLSSBackend must be one feeder or aio entry";
                    return false;
                }
                backend_seen = true;
                backend = value;
            }
            values[line.substr(0, equals)] = line.substr(equals + 1);
        }
    }
    struct Key { const char* Name; std::wstring* Value; bool Folder; bool MustExist; };
    const Key keys[] = {{"Map", &config.Map, false, true}, {"Downloads", &config.Downloads, true, true},
                        {"Shortcut", &config.Shortcut, false, false}, {"GamePaks", &config.GamePaks, true, true}};
    for (const Key& key : keys) {
        auto it = values.find(key.Name);
        if (it == values.end() || it->second.empty()) {
            error = std::string("TextureQuest.ini has no ") + key.Name + " entry";
            return false;
        }
        std::wstring value = Widen(it->second);
        const DWORD attributes = GetFileAttributesW(value.c_str());
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES && bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == key.Folder;
        if (key.MustExist && !exists) {
            error = std::string("TextureQuest.ini ") + key.Name + " path does not exist";
            return false;
        }
        if (key.Folder && value.back() != L'\\')
            value += L'\\';
        *key.Value = value;
    }
    config.Effects = effects;
    config.DLSS = dlss;
    config.DLSSBackend = backend;
    return true;
}

}
