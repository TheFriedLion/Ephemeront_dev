#include "settings_ui.h"

#include "app.h"
#include "capture.h"
#include "settings.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kClass[] = L"Ephemeront.Settings";

enum ControlId {
    ID_QUICK_HOTKEY = 100,
    ID_PICKER_HOTKEY,
    ID_QUICK_SECONDS,
    ID_BUFFER_SECONDS,
    ID_SAMPLE_RATE,
    ID_MIC_MONO,
    ID_FOLDER,
    ID_BROWSE,
    ID_APPS,
    ID_SYSTEM,
    ID_MIC,
    ID_SKIP_SILENT,
    ID_NOTIFY,
    ID_STARTUP,
};

HWND g_hwnd = nullptr;
HFONT g_font = nullptr;
int g_dpi = 96;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// The hotkey control uses HOTKEYF_* bits; RegisterHotKey uses MOD_* bits.
WORD ToControl(const Hotkey& hk) {
    BYTE f = 0;
    if (hk.mods & MOD_SHIFT) f |= HOTKEYF_SHIFT;
    if (hk.mods & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (hk.mods & MOD_ALT) f |= HOTKEYF_ALT;
    return MAKEWORD(hk.vk, f);
}

Hotkey FromControl(WORD value) {
    Hotkey hk;
    hk.vk = LOBYTE(value);
    BYTE f = HIBYTE(value);
    if (f & HOTKEYF_SHIFT) hk.mods |= MOD_SHIFT;
    if (f & HOTKEYF_CONTROL) hk.mods |= MOD_CONTROL;
    if (f & HOTKEYF_ALT) hk.mods |= MOD_ALT;
    return hk;
}

HWND Item(int id) { return GetDlgItem(g_hwnd, id); }

int ReadNumber(int id, int fallback) {
    wchar_t buf[16] = {};
    GetWindowTextW(Item(id), buf, _countof(buf));
    return buf[0] ? _wtoi(buf) : fallback;
}

void BrowseFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Save clips to");
    if (SUCCEEDED(dialog->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) SetWindowTextW(Item(ID_FOLDER), path);
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dialog->Release();
}

void Apply() {
    Settings s = g_settings;
    s.quickSave = FromControl(static_cast<WORD>(SendMessageW(Item(ID_QUICK_HOTKEY), HKM_GETHOTKEY, 0, 0)));
    s.picker = FromControl(static_cast<WORD>(SendMessageW(Item(ID_PICKER_HOTKEY), HKM_GETHOTKEY, 0, 0)));
    s.bufferSeconds = std::clamp(ReadNumber(ID_BUFFER_SECONDS, s.bufferSeconds), 5, 3600);
    s.quickSaveSeconds = std::clamp(ReadNumber(ID_QUICK_SECONDS, s.quickSaveSeconds), 1, s.bufferSeconds);
    s.fixed48k = SendMessageW(Item(ID_SAMPLE_RATE), CB_GETCURSEL, 0, 0) == 1;
    s.micMono = std::clamp(static_cast<int>(SendMessageW(Item(ID_MIC_MONO), CB_GETCURSEL, 0, 0)), 0, 2);

    wchar_t folder[MAX_PATH * 2] = {};
    GetWindowTextW(Item(ID_FOLDER), folder, _countof(folder));
    s.outputDir = folder[0] ? folder : DefaultClipsDir();

    auto checked = [](int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; };
    s.captureApps = checked(ID_APPS);
    s.captureSystem = checked(ID_SYSTEM);
    s.captureMic = checked(ID_MIC);
    s.skipSilent = checked(ID_SKIP_SILENT);
    s.notify = checked(ID_NOTIFY);

    if (s.quickSave.vk && s.quickSave.mods == s.picker.mods && s.quickSave.vk == s.picker.vk) {
        MessageBoxW(g_hwnd, L"Quick Save and Clip Picker need different hotkeys.", L"Ephemeront", MB_ICONWARNING);
        return;
    }

    g_settings = s;
    SettingsSave();
    if (checked(ID_STARTUP) != StartupEnabled()) SetStartupEnabled(checked(ID_STARTUP));
    AppSettingsChanged();
    DestroyWindow(g_hwnd);
}

void Build(HWND hwnd, HINSTANCE instance) {
    const int labelX = S(16), labelW = S(190), fieldX = S(214), fieldW = S(220), rowH = S(24), step = S(32);
    int y = S(16);

    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int yy, int w, int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, yy, w, h, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        return c;
    };
    auto label = [&](const wchar_t* text) { add(L"STATIC", text, SS_CENTERIMAGE, labelX, y, labelW, rowH, -1); };
    auto hotkey = [&](const wchar_t* text, int id, const Hotkey& hk) {
        label(text);
        HWND c = add(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, fieldX, y, fieldW, rowH, id);
        SendMessageW(c, HKM_SETHOTKEY, ToControl(hk), 0);
        y += step;
    };
    auto number = [&](const wchar_t* text, int id, int value, const wchar_t* hint) {
        label(text);
        add(L"EDIT", std::to_wstring(value).c_str(), WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT, fieldX, y, S(70),
            rowH, id);
        add(L"STATIC", hint, SS_CENTERIMAGE, fieldX + S(80), y, fieldW - S(80), rowH, -1);
        y += step;
    };
    auto check = [&](const wchar_t* text, int id, bool on) {
        HWND c = add(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, labelX, y, S(420), S(22), id);
        SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        y += S(26);
    };

    const Settings& s = g_settings;
    hotkey(L"Quick Save hotkey", ID_QUICK_HOTKEY, s.quickSave);
    hotkey(L"Clip Picker hotkey", ID_PICKER_HOTKEY, s.picker);
    number(L"Quick Save length", ID_QUICK_SECONDS, s.quickSaveSeconds, L"seconds");
    number(L"Buffer length", ID_BUFFER_SECONDS, s.bufferSeconds, L"seconds (longest clip)");

    label(L"Sample rate");
    HWND rate = add(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, fieldX, y, fieldW, S(120),
                    ID_SAMPLE_RATE);
    const std::wstring device = L"Match output device (" + std::to_wstring(Capture::DeviceMixRate()) + L" Hz)";
    SendMessageW(rate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.c_str()));
    SendMessageW(rate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"48000 Hz (film and video)"));
    SendMessageW(rate, CB_SETCURSEL, s.fixed48k ? 1 : 0, 0);
    y += step;

    label(L"Microphone input");
    HWND mono = add(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, fieldX, y, fieldW, S(120),
                    ID_MIC_MONO);
    for (const wchar_t* option : {L"Stereo", L"Mono, input 1 (left)", L"Mono, input 2 (right)"}) {
        SendMessageW(mono, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
    }
    SendMessageW(mono, CB_SETCURSEL, s.micMono, 0);
    y += step;

    label(L"Save clips to");
    add(L"EDIT", s.outputDir.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, fieldX, y, fieldW - S(76), rowH,
        ID_FOLDER);
    add(L"BUTTON", L"Browse…", WS_TABSTOP | BS_PUSHBUTTON, fieldX + fieldW - S(70), y, S(70), rowH, ID_BROWSE);
    y += step + S(6);

    check(L"Record each app as its own track", ID_APPS, s.captureApps);
    check(L"Record the full system mix as a track", ID_SYSTEM, s.captureSystem);
    check(L"Record my microphone", ID_MIC, s.captureMic);
    check(L"Skip silent tracks when saving", ID_SKIP_SILENT, s.skipSilent);
    check(L"Show a notification after saving", ID_NOTIFY, s.notify);
    check(L"Start Ephemeront with Windows", ID_STARTUP, StartupEnabled());

    y += S(6);
    add(L"STATIC",
        L"Memory: about 23 MB per minute of buffer, per track, only while that track is making sound. "
        L"Changing the buffer length or sample rate clears what's recorded so far.",
        0, labelX, y, S(418), S(54), -1);
    y += S(66);

    add(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, S(250), y, S(88), S(28), IDOK);
    add(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, S(346), y, S(88), S(28), IDCANCEL);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: Apply(); return 0;
                case IDCANCEL: DestroyWindow(hwnd); return 0;
                case ID_BROWSE: BrowseFolder(); return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_hwnd = nullptr;
            if (g_font) DeleteObject(g_font);
            g_font = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void SettingsUiOpen(HINSTANCE instance) {
    if (g_hwnd) {
        SetForegroundWindow(g_hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = MakeAppIcon(32);
        wc.hIconSm = MakeAppIcon(16);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    g_hwnd = CreateWindowExW(0, kClass, L"Ephemeront Settings", style, cursor.x, cursor.y, 100, 100, nullptr, nullptr,
                             instance, nullptr);
    if (!g_hwnd) return;
    g_dpi = GetDpiForWindow(g_hwnd);

    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    Build(g_hwnd, instance);

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    RECT rc = {0, 0, S(450), S(518)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const RECT& work = mi.rcWork;
    SetWindowPos(g_hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
                 work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

bool SettingsUiIsDialogMessage(MSG* msg) { return g_hwnd && IsDialogMessageW(g_hwnd, msg); }
