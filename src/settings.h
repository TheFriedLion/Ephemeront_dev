#pragma once

#include "common.h"

struct Hotkey {
    UINT mods = 0;  // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN
    UINT vk = 0;
};

struct Settings {
    Hotkey quickSave{MOD_CONTROL | MOD_ALT, 'S'};
    Hotkey picker{MOD_CONTROL | MOD_ALT, 'C'};
    int quickSaveSeconds = 30;
    int bufferSeconds = 120;
    bool fixed48k = false;  // record at 48 kHz instead of the output device's rate
    std::wstring outputDir;
    bool captureApps = true;
    bool captureSystem = true;
    bool captureMic = false;
    int micMono = 0;  // 0 = stereo, 1 = mono from input 1 (left), 2 = mono from input 2 (right)
    bool skipSilent = true;
    bool notify = true;
};

extern Settings g_settings;

void SettingsLoad();
void SettingsSave();

bool StartupEnabled();
void SetStartupEnabled(bool enabled);

std::wstring HotkeyToString(const Hotkey& hk);
