#include "settings.h"

#include <algorithm>

#pragma comment(lib, "advapi32.lib")

Settings g_settings;

namespace {

constexpr wchar_t kSection[] = L"Ephemeront";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Ephemeront";

std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

int ReadInt(const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, IniPath().c_str()));
}

void WriteInt(const wchar_t* key, int value) {
    WritePrivateProfileStringW(kSection, key, std::to_wstring(value).c_str(), IniPath().c_str());
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

}  // namespace

void SettingsLoad() {
    Settings d;
    Settings& s = g_settings;
    s.quickSave.mods = ReadInt(L"QuickSaveModifiers", d.quickSave.mods);
    s.quickSave.vk = ReadInt(L"QuickSaveKey", d.quickSave.vk);
    s.picker.mods = ReadInt(L"PickerModifiers", d.picker.mods);
    s.picker.vk = ReadInt(L"PickerKey", d.picker.vk);
    s.quickSaveSeconds = std::clamp(ReadInt(L"QuickSaveSeconds", d.quickSaveSeconds), 1, 3600);
    s.bufferSeconds = std::clamp(ReadInt(L"BufferSeconds", d.bufferSeconds), 5, 3600);
    s.quickSaveSeconds = std::min(s.quickSaveSeconds, s.bufferSeconds);
    s.fixed48k = ReadInt(L"SampleRate", 0) == 48000;
    s.captureApps = ReadInt(L"CaptureApps", d.captureApps) != 0;
    s.captureSystem = ReadInt(L"CaptureSystem", d.captureSystem) != 0;
    s.captureMic = ReadInt(L"CaptureMic", d.captureMic) != 0;
    s.micMono = std::clamp(ReadInt(L"MicMono", d.micMono), 0, 2);
    s.skipSilent = ReadInt(L"SkipSilent", d.skipSilent) != 0;
    s.notify = ReadInt(L"Notify", d.notify) != 0;

    wchar_t dir[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(kSection, L"OutputFolder", L"", dir, _countof(dir), IniPath().c_str());
    s.outputDir = dir[0] ? dir : DefaultClipsDir();
}

void SettingsSave() {
    const Settings& s = g_settings;
    WriteInt(L"QuickSaveModifiers", s.quickSave.mods);
    WriteInt(L"QuickSaveKey", s.quickSave.vk);
    WriteInt(L"PickerModifiers", s.picker.mods);
    WriteInt(L"PickerKey", s.picker.vk);
    WriteInt(L"QuickSaveSeconds", s.quickSaveSeconds);
    WriteInt(L"BufferSeconds", s.bufferSeconds);
    WriteInt(L"SampleRate", s.fixed48k ? 48000 : 0);  // 0 = match the output device
    WriteInt(L"CaptureApps", s.captureApps);
    WriteInt(L"CaptureSystem", s.captureSystem);
    WriteInt(L"CaptureMic", s.captureMic);
    WriteInt(L"MicMono", s.micMono);
    WriteInt(L"SkipSilent", s.skipSilent);
    WriteInt(L"Notify", s.notify);
    WritePrivateProfileStringW(kSection, L"OutputFolder", s.outputDir.c_str(), IniPath().c_str());
}

bool StartupEnabled() {
    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return false;
    return wcsstr(value, ExePath().c_str()) != nullptr;
}

void SetStartupEnabled(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

std::wstring HotkeyToString(const Hotkey& hk) {
    if (!hk.vk) return L"(none)";
    std::wstring s;
    if (hk.mods & MOD_CONTROL) s += L"Ctrl+";
    if (hk.mods & MOD_ALT) s += L"Alt+";
    if (hk.mods & MOD_SHIFT) s += L"Shift+";
    if (hk.mods & MOD_WIN) s += L"Win+";

    UINT scan = MapVirtualKeyW(hk.vk, MAPVK_VK_TO_VSC);
    switch (hk.vk) {
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 0x100;  // extended key
            break;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, _countof(name)) > 0) {
        s += name;
    } else {
        s += L"Key " + std::to_wstring(hk.vk);
    }
    return s;
}
