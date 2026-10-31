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
};

inline std::wstring Widen(const std::string& text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), int(wide.size()));
    return wide;
}

inline bool ReadConfig(const std::wstring& ini, Config& config, std::string& error) {
    std::ifstream file(ini, std::ios::binary);
    if (!file) {
        error = "TextureQuest.ini not found next to the add-on";
        return false;
    }
    std::map<std::string, std::string> values;
    bool section = false;
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
        if (section && equals != std::string::npos)
            values[line.substr(0, equals)] = line.substr(equals + 1);
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
    return true;
}

}
