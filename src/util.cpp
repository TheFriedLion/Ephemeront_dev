#include "common.h"

#include <shlobj.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ---- Clock ------------------------------------------------------------------

static int g_sampleRate = 48000;
static LARGE_INTEGER g_qpcFreq;
static LARGE_INTEGER g_qpcStart;
static int64_t g_start100ns;

static int64_t TicksTo(int64_t ticks, int64_t unitsPerSecond) {
    // Split into whole seconds and remainder so the multiply can't overflow.
    return (ticks / g_qpcFreq.QuadPart) * unitsPerSecond +
           (ticks % g_qpcFreq.QuadPart) * unitsPerSecond / g_qpcFreq.QuadPart;
}

void SampleRateInit(int rate) { g_sampleRate = rate; }
int SampleRate() { return g_sampleRate; }

void ClockInit() {
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    g_start100ns = TicksTo(g_qpcStart.QuadPart, 10000000);
}

int64_t NowFrame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return TicksTo(now.QuadPart - g_qpcStart.QuadPart, g_sampleRate);
}

int64_t QpcToFrame(uint64_t qpc100ns) {
    int64_t elapsed = static_cast<int64_t>(qpc100ns) - g_start100ns;
    return elapsed * g_sampleRate / 10000000;
}

// ---- Log --------------------------------------------------------------------

static std::mutex g_logMutex;
static FILE* g_log = nullptr;

void LogInit(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);  // readable while running
}

void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = swprintf_s(line, L"%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vswprintf_s(line + n, _countof(line) - n, fmt, args);
    va_end(args);

    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) {
        fwprintf(g_log, L"%s\n", line);
        fflush(g_log);
    }
}

// ---- Paths ------------------------------------------------------------------

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) path = raw;
    CoTaskMemFree(raw);
    return path;
}

std::wstring AppDataDir() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\Ephemeront";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring DefaultClipsDir() {
    return KnownFolder(FOLDERID_Music) + L"\\Ephemeront";
}

std::wstring SanitizeFileName(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        out += c;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out.empty() ? L"Audio" : out;
}

std::wstring TimestampForFile() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// ---- Audio ------------------------------------------------------------------

float PeakOf(const float* samples, size_t count) {
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > peak) peak = v;
    }
    return peak;
}

// ---- Icon -------------------------------------------------------------------

// Draws five rounded "level meter" bars in gold, antialiased by 4x supersampling.
HICON MakeAppIcon(int size) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    const float heights[5] = {0.38f, 0.72f, 1.0f, 0.6f, 0.32f};
    const float barW = 0.12f, gap = 0.06f, margin = 0.08f, span = 0.84f;
    const int ss = 4;
    auto* px = static_cast<uint32_t*>(bits);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    float u = (x + (sx + 0.5f) / ss) / size;
                    float v = (y + (sy + 0.5f) / ss) / size;
                    for (int b = 0; b < 5; ++b) {
                        float left = margin + b * (barW + gap);
                        float r = barW / 2;
                        float cx = left + r;
                        float half = heights[b] * span / 2;
                        float top = 0.5f - half + r, bottom = 0.5f + half - r;
                        float dy = v < top ? top - v : (v > bottom ? v - bottom : 0.0f);
                        float dx = u - cx;
                        if (dx * dx + dy * dy <= r * r) { ++hits; break; }
                    }
                }
            }
            uint32_t a = static_cast<uint32_t>(hits * 255 / (ss * ss));
            px[y * size + x] = (a << 24) | (0xF2 << 16) | (0xA9 << 8) | 0x3B;
        }
    }

    std::vector<BYTE> maskBits(((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
