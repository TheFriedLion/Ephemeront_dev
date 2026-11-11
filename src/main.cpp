// Ephemeront: a tray app that keeps a rolling recording of your audio and saves
// the last few seconds on a hotkey.

#include "app.h"
#include "capture.h"
#include "picker.h"
#include "settings.h"
#include "settings_ui.h"
#include "wav.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <map>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int HK_QUICK = 1;
constexpr int HK_PICKER = 2;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kTooltipTimer = 1;

enum MenuId { MENU_PICKER = 1, MENU_QUICK, MENU_FOLDER, MENU_MIC, MENU_SETTINGS, MENU_EXIT };

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HICON g_icon = nullptr;
UINT g_taskbarCreated = 0;
bool g_balloonOpensFolder = false;

Capture::Config ConfigFromSettings() {
    Capture::Config c;
    c.apps = g_settings.captureApps;
    c.system = g_settings.captureSystem;
    c.mic = g_settings.captureMic;
    c.micMono = g_settings.micMono;
    c.bufferSeconds = g_settings.bufferSeconds;
    return c;
}

NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    return nid;
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icon;
    wcscpy_s(nid.szTip, L"Ephemeront");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = TrayData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Balloon(const wchar_t* title, const std::wstring& text, bool opensFolder = false) {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    g_balloonOpensFolder = opensFolder;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void UpdateTooltip() {
    int sources = 0;
    size_t bytes = 0;
    Capture::Stats(&sources, &bytes);
    wchar_t tip[128];
    swprintf_s(tip, L"Ephemeront\nListening to %d track%s · %.0f MB", sources, sources == 1 ? L"" : L"s",
               bytes / (1024.0 * 1024.0));
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void RegisterHotkeys() {
    UnregisterHotKey(g_hwnd, HK_QUICK);
    UnregisterHotKey(g_hwnd, HK_PICKER);
    std::wstring taken;
    auto reg = [&](int id, const Hotkey& hk) {
        if (!hk.vk) return;
        if (!RegisterHotKey(g_hwnd, id, hk.mods | MOD_NOREPEAT, hk.vk)) {
            if (!taken.empty()) taken += L" and ";
            taken += HotkeyToString(hk);
        }
    };
    reg(HK_QUICK, g_settings.quickSave);
    reg(HK_PICKER, g_settings.picker);
    if (!taken.empty()) {
        Log(L"Hotkey unavailable: %s", taken.c_str());
        Balloon(L"Hotkey unavailable", taken + L" is already used by another app. Pick a different one in Settings.");
    }
}

void QuickSave() {
    const int64_t frames = static_cast<int64_t>(std::min(g_settings.quickSaveSeconds, g_settings.bufferSeconds)) * SampleRate();
    const int64_t end = NowFrame() - TailFrames();
    std::vector<Clip> clips = Capture::Grab(end - frames, frames, g_settings.skipSilent);
    if (clips.empty()) {
        if (g_settings.notify) Balloon(L"Nothing to save", L"Ephemeront hasn't heard anything in that time.");
        return;
    }
    std::vector<const Clip*> chosen;
    for (const Clip& c : clips) chosen.push_back(&c);
    AppNotifySaved(SaveClips(chosen, 0, frames, false), frames / double(SampleRate()));
}

void OpenPicker() {
    if (PickerIsOpen()) {
        PickerFocus();
        return;
    }
    if (!PickerOpen(g_instance, static_cast<int64_t>(g_settings.quickSaveSeconds) * SampleRate())) {
        Balloon(L"Nothing to clip", L"Ephemeront hasn't heard anything yet.");
    }
}

void OpenClipsFolder() {
    SHCreateDirectoryExW(nullptr, g_settings.outputDir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", g_settings.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RunCommand(WPARAM cmd) {
    switch (cmd) {
        case CMD_QUICK_SAVE: QuickSave(); break;
        case CMD_PICKER: OpenPicker(); break;
        case CMD_SETTINGS: SettingsUiOpen(g_instance); break;
        case CMD_EXIT: DestroyWindow(g_hwnd); break;
    }
}

void ShowMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    std::wstring picker = L"Clip Picker…\t" + HotkeyToString(g_settings.picker);
    std::wstring quick = L"Quick Save last " + std::to_wstring(g_settings.quickSaveSeconds) + L" s\t" +
                         HotkeyToString(g_settings.quickSave);
    AppendMenuW(menu, MF_STRING, MENU_PICKER, picker.c_str());
    AppendMenuW(menu, MF_STRING, MENU_QUICK, quick.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MENU_FOLDER, L"Open clips folder");
    AppendMenuW(menu, MF_STRING | (g_settings.captureMic ? MF_CHECKED : 0), MENU_MIC, L"Record microphone");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MENU_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, MENU_EXIT, L"Exit");
    SetMenuDefaultItem(menu, MENU_PICKER, FALSE);

    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case MENU_PICKER: OpenPicker(); break;
        case MENU_QUICK: QuickSave(); break;
        case MENU_FOLDER: OpenClipsFolder(); break;
        case MENU_MIC:
            g_settings.captureMic = !g_settings.captureMic;
            SettingsSave();
            Capture::Configure(ConfigFromSettings());
            break;
        case MENU_SETTINGS: SettingsUiOpen(g_instance); break;
        case MENU_EXIT: DestroyWindow(g_hwnd); break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon();  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_HOTKEY:
            if (wParam == HK_QUICK) QuickSave();
            if (wParam == HK_PICKER) OpenPicker();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case WM_CONTEXTMENU: ShowMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)}); break;
                case NIN_SELECT:
                case NIN_KEYSELECT: OpenPicker(); break;
                case NIN_BALLOONUSERCLICK:
                    if (g_balloonOpensFolder) OpenClipsFolder();
                    break;
            }
            return 0;
        case WM_APP_COMMAND:
            RunCommand(wParam);
            return 0;
        case WM_TIMER:
            if (wParam == kTooltipTimer) UpdateTooltip();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

WPARAM ParseCommand() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WPARAM cmd = 0;
    for (int i = 1; argv && i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--save")) cmd = CMD_QUICK_SAVE;
        else if (!_wcsicmp(argv[i], L"--pick")) cmd = CMD_PICKER;
        else if (!_wcsicmp(argv[i], L"--settings")) cmd = CMD_SETTINGS;
        else if (!_wcsicmp(argv[i], L"--exit")) cmd = CMD_EXIT;
    }
    LocalFree(argv);
    return cmd;
}

}  // namespace

// ---- Functions shared with the other windows --------------------------------

int SaveClips(const std::vector<const Clip*>& clips, int64_t start, int64_t frames, bool skipSilent) {
    const std::wstring& dir = g_settings.outputDir;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring stamp = TimestampForFile();
    std::map<std::wstring, int> used;
    int saved = 0;
    for (const Clip* clip : clips) {
        if (start < 0 || start + frames > clip->Frames()) continue;
        const float* pcm = clip->pcm.data() + start * kChannels;
        if (skipSilent && PeakOf(pcm, static_cast<size_t>(frames) * kChannels) <= kSilentPeak) continue;

        std::wstring name = SanitizeFileName(clip->name);
        int& count = used[name];
        if (++count > 1) name += L" " + std::to_wstring(count);
        std::wstring base = dir + L"\\" + stamp + L" " + name;
        std::wstring path = base + L".wav";
        for (int k = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++k) {
            path = base + L" (" + std::to_wstring(k) + L").wav";
        }
        if (WriteWav(path, pcm, static_cast<size_t>(frames))) ++saved;
    }
    Log(L"Saved %d file(s), %.1f s each, to %s", saved, frames / double(SampleRate()), dir.c_str());
    return saved;
}

void AppNotifySaved(int files, double seconds) {
    if (!g_settings.notify) return;
    if (files == 0) {
        Balloon(L"Nothing saved", L"The chosen tracks were silent over that stretch.");
        return;
    }
    wchar_t text[128];
    swprintf_s(text, L"%d track%s, %.1f s. Click to open the folder.", files, files == 1 ? L"" : L"s", seconds);
    Balloon(L"Clip saved", text, true);
}

int ChosenSampleRate() { return g_settings.fixed48k ? 48000 : Capture::DeviceMixRate(); }

void AppSettingsChanged() {
    RegisterHotkeys();
    if (ChosenSampleRate() != SampleRate()) {
        // Every buffer and the shared clock are measured in samples, so a new rate
        // means starting the recording over.
        PickerClose();
        Capture::Stop();
        SampleRateInit(ChosenSampleRate());
        ClockInit();
        Log(L"Now recording at %d Hz", SampleRate());
        Capture::Start(ConfigFromSettings());
    } else {
        Capture::Configure(ConfigFromSettings());
    }
}

// ---- Entry point ------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const WPARAM command = ParseCommand();

    // One copy at a time. Launching it again forwards the command to the running copy
    // (or opens Settings), which also lets macro pads and scripts drive it.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\Ephemeront.Running");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kMainWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_COMMAND, command ? command : CMD_SETTINGS, 0);
        }
        return 0;
    }
    if (command == CMD_EXIT) return 0;

    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring dataDir = AppDataDir();
    LogInit(dataDir + L"\\log.txt");
    const bool firstRun = GetFileAttributesW((dataDir + L"\\settings.ini").c_str()) == INVALID_FILE_ATTRIBUTES;
    SettingsLoad();
    if (firstRun) SettingsSave();

    SampleRateInit(ChosenSampleRate());
    ClockInit();
    Log(L"Ephemeront starting, recording 32-bit float at %d Hz", SampleRate());

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_icon = MakeAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMainWindowClass;
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, kMainWindowClass, L"Ephemeront", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                             nullptr);

    AddTrayIcon();
    RegisterHotkeys();
    Capture::Start(ConfigFromSettings());
    SetTimer(g_hwnd, kTooltipTimer, 3000, nullptr);

    if (firstRun) {
        Balloon(L"Ephemeront is listening",
                HotkeyToString(g_settings.quickSave) + L" saves the last " + std::to_wstring(g_settings.quickSaveSeconds) +
                    L" s. " + HotkeyToString(g_settings.picker) + L" opens the Clip Picker.");
    }
    if (command) PostMessageW(g_hwnd, WM_APP_COMMAND, command, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (SettingsUiIsDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Capture::Stop();
    RemoveTrayIcon();
    CoUninitialize();
    CloseHandle(single);
    Log(L"Ephemeront stopped");
    return 0;
}
