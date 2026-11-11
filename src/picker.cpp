#include "picker.h"

#include "app.h"
#include "capture.h"
#include "settings.h"
#include "wav.h"

#include <dwmapi.h>
#include <mmsystem.h>
#include <windowsx.h>

#include <algorithm>
#include <deque>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kClass[] = L"Ephemeront.Picker";
constexpr UINT_PTR kLiveTimer = 1;
constexpr UINT kLiveMs = 50;      // 20 updates a second, only while the window is open
constexpr int64_t kBlock = 256;   // frames summarized by each waveform peak

constexpr COLORREF kBg = RGB(24, 24, 28);
constexpr COLORREF kLaneBg = RGB(34, 34, 41);
constexpr COLORREF kSelBg = RGB(52, 54, 66);
constexpr COLORREF kText = RGB(232, 232, 236);
constexpr COLORREF kMuted = RGB(140, 140, 150);
constexpr COLORREF kOffWave = RGB(78, 78, 86);
constexpr COLORREF kAccent = RGB(242, 169, 59);
constexpr COLORREF kAccentHover = RGB(255, 190, 92);
constexpr COLORREF kLive = RGB(236, 84, 84);
constexpr COLORREF kButton = RGB(46, 46, 54);
constexpr COLORREF kButtonHover = RGB(64, 64, 74);
constexpr COLORREF kLaneColors[] = {RGB(242, 169, 59),  RGB(96, 178, 255),  RGB(120, 214, 150), RGB(236, 112, 140),
                                    RGB(178, 140, 255), RGB(90, 210, 210),  RGB(240, 220, 110), RGB(255, 140, 90)};

enum ButtonId { BTN_NONE, BTN_5, BTN_10, BTN_30, BTN_60, BTN_ALL, BTN_PREVIEW, BTN_SAVE, BTN_CANCEL };

struct Button {
    ButtonId id;
    const wchar_t* label;
    RECT rc;
};

struct Peak {
    float lo, hi;
};

struct Lane {
    uint64_t id = 0;
    std::wstring name;
    bool on = true;
    COLORREF color = 0;
    int64_t firstBlock = 0;   // absolute block index of peaks.front()
    std::deque<Peak> peaks;   // waveform summary, one entry per kBlock frames
    RECT rc{}, check{};
};

enum class Drag { None, New, EdgeA, EdgeB };

struct State {
    HWND hwnd = nullptr;
    std::vector<Lane> lanes;
    int64_t windowFrames = 0;           // buffer length
    int64_t end = 0;                    // "now": everything before this is summarized
    int64_t firstSound = INT64_MAX;     // earliest sound seen, for trimming the view
    int64_t selA = 0, selB = 0;         // selection, in absolute frames
    int colors = 0;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr, tiny = nullptr;
    RECT header{}, ruler{}, wave{};
    std::vector<Button> buttons;
    ButtonId hover = BTN_NONE, pressed = BTN_NONE;
    Drag drag = Drag::None;
    int64_t anchor = 0, prevA = 0, prevB = 0;
    std::vector<uint8_t> previewWav;
    bool playing = false;
    ULONGLONG playStart = 0;
    int64_t playFrom = 0, playLen = 0;
    std::vector<float> scratch;
    std::vector<float> colTop, colBottom;
    std::vector<POINT> segPoints[3];  // waveform columns, batched by color
    HDC backDc = nullptr;             // off-screen image, reused between frames
    HBITMAP backBmp = nullptr;
    SIZE backSize = {};
};

State* g = nullptr;

int S(int v) { return MulDiv(v, g->dpi, 96); }

int64_t AlignDown(int64_t f) { return f - ((f % kBlock) + kBlock) % kBlock; }

COLORREF Dim(COLORREF c) { return RGB(GetRValue(c) * 45 / 100, GetGValue(c) * 45 / 100, GetBValue(c) * 45 / 100); }

void Fill(HDC dc, const RECT& rc, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void FillRound(HDC dc, const RECT& rc, COLORREF color, int radius) {
    SetDCBrushColor(dc, color);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void Text(HDC dc, HFONT font, COLORREF color, const wchar_t* text, RECT rc, UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rc, flags | DT_SINGLELINE | DT_NOPREFIX);
}

void MakeFonts() {
    for (HFONT f : {g->font, g->bold, g->tiny}) {
        if (f) DeleteObject(f);
    }
    auto make = [](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, g->dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    g->font = make(10, FW_NORMAL);
    g->bold = make(11, FW_SEMIBOLD);
    g->tiny = make(8, FW_NORMAL);
}

// ---- Timeline ---------------------------------------------------------------

int64_t WindowStart() { return g->end - g->windowFrames; }

// The visible range ends at "now". Silence before the first sound is hidden, but
// at least 5 seconds are always shown.
int64_t ViewStart() {
    const int64_t rate = SampleRate();
    int64_t v = WindowStart();
    if (g->firstSound != INT64_MAX) v = std::max(v, g->firstSound - rate / 2);
    v = std::min(v, g->end - 5 * rate);
    return AlignDown(std::max(v, WindowStart()));
}

int64_t ViewLen() { return std::max<int64_t>(1, g->end - ViewStart()); }

int WaveWidth() { return std::max<int>(1, g->wave.right - g->wave.left); }

int XOf(int64_t frame) {
    return g->wave.left + static_cast<int>((frame - ViewStart()) * WaveWidth() / ViewLen());
}

int64_t FrameAt(int x) {
    int64_t f = ViewStart() + static_cast<int64_t>(x - g->wave.left) * ViewLen() / WaveWidth();
    return std::clamp(f, ViewStart(), g->end);
}

// Summarizes [start, start + frames) into lane peaks. Returns true if any of it was loud.
bool AppendPeaks(Lane& lane, const float* pcm, int64_t start, int64_t frames) {
    const int64_t startBlock = start / kBlock;
    if (lane.peaks.empty()) lane.firstBlock = startBlock;
    while (lane.firstBlock + static_cast<int64_t>(lane.peaks.size()) < startBlock) lane.peaks.push_back({0, 0});

    bool loud = false;
    for (int64_t b = 0; b < frames / kBlock; ++b) {
        const float* p = pcm + b * kBlock * kChannels;
        float lo = 0, hi = 0;
        for (int64_t i = 0; i < kBlock * kChannels; ++i) {
            lo = std::min(lo, p[i]);
            hi = std::max(hi, p[i]);
        }
        lane.peaks.push_back({lo, hi});
        if (hi > kSilentPeak || lo < -kSilentPeak) {
            loud = true;
            g->firstSound = std::min(g->firstSound, start + b * kBlock);
        }
    }
    return loud;
}

// Pulls new audio for [from, to) from every source. Sources that make sound for
// the first time get a lane.
bool Scan(int64_t from, int64_t to) {
    bool added = false;
    const int64_t piece = AlignDown(SampleRate());
    for (const Capture::SourceInfo& src : Capture::Sources()) {
        auto it = std::find_if(g->lanes.begin(), g->lanes.end(), [&](const Lane& l) { return l.id == src.id; });
        if (it == g->lanes.end() && src.lastSoundFrame <= from) continue;

        Lane fresh;
        fresh.id = src.id;
        fresh.name = src.name;
        Lane& lane = it != g->lanes.end() ? *it : fresh;
        bool loud = false;
        for (int64_t pos = from; pos < to; pos += piece) {
            int64_t n = std::min(piece, to - pos);
            if (!Capture::Read(src.id, pos, n, g->scratch)) break;
            loud |= AppendPeaks(lane, g->scratch.data(), pos, n);
        }
        if (it == g->lanes.end() && loud) {
            fresh.color = kLaneColors[g->colors++ % _countof(kLaneColors)];
            g->lanes.push_back(std::move(fresh));
            added = true;
        }
    }
    return added;
}

void TrimPeaks() {
    const int64_t oldest = WindowStart() / kBlock;
    for (Lane& lane : g->lanes) {
        while (!lane.peaks.empty() && lane.firstBlock < oldest) {
            lane.peaks.pop_front();
            ++lane.firstBlock;
        }
    }
}

// ---- Layout and painting ----------------------------------------------------

int DesiredClientHeight() { return S(118 + static_cast<int>(g->lanes.size()) * 62); }

void Layout() {
    RECT c;
    GetClientRect(g->hwnd, &c);
    const int m = S(14), nameW = S(160), gap = S(6), footerH = S(32);
    g->header = {m, m, c.right - m, m + S(24)};
    g->ruler = {m + nameW, g->header.bottom + S(8), c.right - m, g->header.bottom + S(24)};

    const int footerTop = c.bottom - m - footerH;
    const int top = g->ruler.bottom + S(4);
    const int n = std::max<int>(1, static_cast<int>(g->lanes.size()));
    int laneH = (footerTop - S(12) - top - gap * (n - 1)) / n;
    laneH = std::clamp(laneH, S(20), S(56));
    for (int i = 0; i < static_cast<int>(g->lanes.size()); ++i) {
        Lane& lane = g->lanes[i];
        int y = top + i * (laneH + gap);
        lane.rc = {m, y, c.right - m, y + laneH};
        int mid = y + laneH / 2;
        lane.check = {m + S(10), mid - S(8), m + S(26), mid + S(8)};
    }
    g->wave = {m + nameW, top, c.right - m, top + n * (laneH + gap) - gap};

    g->buttons.clear();
    const struct { ButtonId id; const wchar_t* label; } presets[] = {
        {BTN_5, L"5s"}, {BTN_10, L"10s"}, {BTN_30, L"30s"}, {BTN_60, L"60s"}, {BTN_ALL, L"All"}};
    int x = m;
    for (auto& p : presets) {
        g->buttons.push_back({p.id, p.label, {x, footerTop, x + S(46), footerTop + footerH}});
        x += S(46) + gap;
    }
    int right = c.right - m;
    const struct { ButtonId id; const wchar_t* label; int w; } actions[] = {
        {BTN_CANCEL, L"Cancel", 84}, {BTN_SAVE, L"Save", 84}, {BTN_PREVIEW, L"▶ Preview", 100}};
    for (auto& a : actions) {
        g->buttons.push_back({a.id, a.label, {right - S(a.w), footerTop, right, footerTop + footerH}});
        right -= S(a.w) + gap;
    }
}

// Grows the window when a new track appears, if there's room on screen.
void FitHeight() {
    RECT client, frame;
    GetClientRect(g->hwnd, &client);
    if (client.bottom >= DesiredClientHeight() || IsZoomed(g->hwnd)) return;
    GetWindowRect(g->hwnd, &frame);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int extra = DesiredClientHeight() - client.bottom;
    int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 85 / 100;
    int h = std::min<int>(frame.bottom - frame.top + extra, maxH);
    int top = std::min<int>(frame.top, mi.rcWork.bottom - h);
    SetWindowPos(g->hwnd, nullptr, frame.left, top, frame.right - frame.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

int CountOn() {
    int on = 0;
    for (const Lane& lane : g->lanes) on += lane.on;
    return on;
}

void DrawRuler(HDC dc) {
    const double span = static_cast<double>(ViewLen()) / SampleRate();
    const int steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
    int step = steps[0];
    for (int s : steps) {
        step = s;
        if (span / s <= 8) break;
    }
    for (int k = 0; k * step <= span; ++k) {
        int x = XOf(g->end - static_cast<int64_t>(k) * step * SampleRate());
        Fill(dc, {x, g->ruler.bottom - S(4), x + 1, g->ruler.bottom}, kMuted);
        wchar_t label[16];
        if (k == 0) {
            wcscpy_s(label, L"now");
        } else if (step >= 60 && (k * step) % 60 == 0) {
            swprintf_s(label, L"-%dm", k * step / 60);
        } else {
            swprintf_s(label, L"-%ds", k * step);
        }
        RECT rc = {x - S(40), g->ruler.top, x - S(3), g->ruler.bottom - S(2)};
        if (rc.left < g->ruler.left - S(10)) continue;
        Text(dc, g->tiny, kMuted, label, rc, DT_RIGHT | DT_VCENTER);
    }
}

void DrawLane(HDC dc, const Lane& lane, int xa, int xb) {
    Fill(dc, lane.rc, kLaneBg);
    Fill(dc, {xa, lane.rc.top, xb, lane.rc.bottom}, kSelBg);

    // Checkbox
    if (lane.on) {
        FillRound(dc, lane.check, lane.color, S(5));
        HPEN pen = CreatePen(PS_SOLID, S(2), kBg);
        HGDIOBJ old = SelectObject(dc, pen);
        const RECT& c = lane.check;
        POINT tick[] = {{c.left + S(4), c.top + S(8)}, {c.left + S(7), c.top + S(11)}, {c.left + S(12), c.top + S(5)}};
        Polyline(dc, tick, 3);
        SelectObject(dc, old);
        DeleteObject(pen);
    } else {
        FillRound(dc, lane.check, kMuted, S(5));
        RECT inner = {lane.check.left + S(2), lane.check.top + S(2), lane.check.right - S(2), lane.check.bottom - S(2)};
        FillRound(dc, inner, kLaneBg, S(3));
    }
    RECT name = {lane.check.right + S(8), lane.rc.top, g->wave.left - S(8), lane.rc.bottom};
    Text(dc, g->font, lane.on ? kText : kMuted, lane.name.c_str(), name, DT_VCENTER | DT_END_ELLIPSIS);

    // Waveform: gather each pixel column's peak, scaled so quiet tracks stay readable.
    const int w = WaveWidth();
    const int64_t vs = ViewStart(), vl = ViewLen();
    const int64_t first = lane.firstBlock, last = first + static_cast<int64_t>(lane.peaks.size());
    g->colTop.assign(w, 0.0f);
    g->colBottom.assign(w, 0.0f);
    float norm = 0.1f;
    for (int col = 0; col < w; ++col) {
        int64_t b0 = (vs + col * vl / w) / kBlock;
        int64_t b1 = std::max(b0 + 1, (vs + (col + 1) * vl / w + kBlock - 1) / kBlock);
        float hi = 0, lo = 0;
        for (int64_t b = std::max(b0, first); b < std::min(b1, last); ++b) {
            const Peak& p = lane.peaks[static_cast<size_t>(b - first)];
            hi = std::max(hi, p.hi);
            lo = std::min(lo, p.lo);
        }
        g->colTop[col] = hi;
        g->colBottom[col] = lo;
        norm = std::max(norm, std::max(hi, -lo));
    }

    // Each column is a short vertical line. Lines are grouped by color and drawn in
    // one call per color, which keeps live redraws cheap.
    const int mid = (lane.rc.top + lane.rc.bottom) / 2;
    const int half = std::max<int>(1, (lane.rc.bottom - lane.rc.top) / 2 - S(4));
    const COLORREF colors[3] = {lane.on ? lane.color : kOffWave, lane.on ? Dim(lane.color) : kOffWave, kOffWave};
    for (auto& pts : g->segPoints) pts.clear();
    for (int col = 0; col < w; ++col) {
        int x = g->wave.left + col;
        int y1 = mid - static_cast<int>(g->colTop[col] / norm * half);
        int y2 = mid - static_cast<int>(g->colBottom[col] / norm * half);
        y1 = std::max<int>(y1, lane.rc.top + S(2));
        y2 = std::min<int>(std::max(y2, y1 + 1), lane.rc.bottom - S(2));
        auto& pts = g->segPoints[(x >= xa && x < xb) ? 0 : 1];
        pts.push_back({x, y1});
        pts.push_back({x, y2});
    }
    for (int i = 0; i < 2; ++i) {
        auto& pts = g->segPoints[i];
        if (pts.empty()) continue;
        std::vector<DWORD> counts(pts.size() / 2, 2);
        HPEN pen = CreatePen(PS_SOLID, 1, colors[i]);
        HGDIOBJ old = SelectObject(dc, pen);
        PolyPolyline(dc, pts.data(), counts.data(), static_cast<DWORD>(counts.size()));
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

void Paint(HDC dc, const RECT& client) {
    Fill(dc, client, kBg);
    SetBkMode(dc, TRANSPARENT);

    // Title, then a "Live" marker so it's obvious the timeline is moving.
    Text(dc, g->bold, kText, L"Clip Picker", g->header, DT_LEFT | DT_VCENTER);
    SIZE title = {};
    SelectObject(dc, g->bold);
    GetTextExtentPoint32W(dc, L"Clip Picker", 11, &title);
    const int dotX = g->header.left + title.cx + S(14);
    const int midY = (g->header.top + g->header.bottom) / 2;
    FillRound(dc, {dotX, midY - S(4), dotX + S(8), midY + S(4)}, kLive, S(8));
    RECT live = {dotX + S(13), g->header.top, dotX + S(80), g->header.bottom};
    Text(dc, g->font, kMuted, L"Live", live, DT_LEFT | DT_VCENTER);

    wchar_t info[96];
    swprintf_s(info, L"%.1f s selected  ·  %d of %d tracks", (g->selB - g->selA) / double(SampleRate()),
               CountOn(), static_cast<int>(g->lanes.size()));
    Text(dc, g->font, kMuted, info, g->header, DT_RIGHT | DT_VCENTER);

    DrawRuler(dc);

    const int xa = std::max<int>(XOf(g->selA), g->wave.left);
    const int xb = std::max(XOf(g->selB), xa + 1);
    for (const Lane& lane : g->lanes) DrawLane(dc, lane, xa, xb);

    const int w = std::max(2, S(2));
    if (g->selA >= ViewStart()) Fill(dc, {xa - w / 2, g->ruler.top, xa - w / 2 + w, g->wave.bottom}, kAccent);
    if (g->selB >= ViewStart()) Fill(dc, {xb - w / 2, g->ruler.top, xb - w / 2 + w, g->wave.bottom}, kAccent);

    if (g->playing) {
        int64_t played = static_cast<int64_t>(GetTickCount64() - g->playStart) * SampleRate() / 1000;
        int x = XOf(g->playFrom + std::min(played, g->playLen));
        if (x >= g->wave.left) Fill(dc, {x, g->wave.top, x + std::max(1, S(1)), g->wave.bottom}, kText);
    }

    int hintLeft = 0, hintRight = client.right;
    for (const Button& b : g->buttons) {
        bool hot = b.id == g->hover;
        COLORREF bg = b.id == BTN_SAVE ? (hot ? kAccentHover : kAccent) : (hot ? kButtonHover : kButton);
        COLORREF fg = b.id == BTN_SAVE ? kBg : kText;
        const wchar_t* label = (b.id == BTN_PREVIEW && g->playing) ? L"■ Stop" : b.label;
        FillRound(dc, b.rc, bg, S(8));
        Text(dc, b.id == BTN_SAVE ? g->bold : g->font, fg, label, b.rc, DT_CENTER | DT_VCENTER);
        if (b.id == BTN_ALL) hintLeft = b.rc.right + S(14);
        if (b.id == BTN_PREVIEW) hintRight = b.rc.left - S(14);
    }
    if (hintRight - hintLeft > S(120)) {
        RECT hint = {hintLeft, g->buttons[0].rc.top, hintRight, g->buttons[0].rc.bottom};
        Text(dc, g->tiny, kMuted, L"Drag to select  ·  Space to preview", hint,
             DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

// ---- Actions ----------------------------------------------------------------

void StopPreview() {
    if (!g->playing) return;
    PlaySoundW(nullptr, nullptr, 0);
    g->playing = false;
}

void TogglePreview() {
    if (g->playing) {
        StopPreview();
        return;
    }
    const int64_t len = g->selB - g->selA;
    if (len <= 0 || CountOn() == 0) return;
    std::vector<float> mix(static_cast<size_t>(len) * kChannels, 0.0f);
    for (const Lane& lane : g->lanes) {
        if (!lane.on || !Capture::Read(lane.id, g->selA, len, g->scratch)) continue;
        for (size_t i = 0; i < mix.size(); ++i) mix[i] += g->scratch[i];
    }
    g->previewWav = MakePreviewWav(mix.data(), static_cast<size_t>(len));
    PlaySoundW(reinterpret_cast<LPCWSTR>(g->previewWav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    g->playing = true;
    g->playStart = GetTickCount64();
    g->playFrom = g->selA;
    g->playLen = len;
}

void Save() {
    StopPreview();
    const int64_t frames = g->selB - g->selA;
    std::vector<Clip> clips;
    for (const Lane& lane : g->lanes) {
        if (!lane.on) continue;
        Clip clip;
        clip.id = lane.id;
        clip.name = lane.name;
        if (Capture::Read(lane.id, g->selA, frames, clip.pcm)) clips.push_back(std::move(clip));
    }
    if (clips.empty() || frames <= 0) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    std::vector<const Clip*> chosen;
    for (const Clip& c : clips) chosen.push_back(&c);
    const int saved = SaveClips(chosen, 0, frames, g_settings.skipSilent);
    DestroyWindow(g->hwnd);  // frees g
    AppNotifySaved(saved, frames / double(SampleRate()));
}

void SelectLast(int seconds) {
    StopPreview();
    g->selB = g->end;
    g->selA = seconds > 0 ? std::max(ViewStart(), g->end - static_cast<int64_t>(seconds) * SampleRate()) : ViewStart();
}

void Activate(ButtonId id) {
    switch (id) {
        case BTN_5: SelectLast(5); break;
        case BTN_10: SelectLast(10); break;
        case BTN_30: SelectLast(30); break;
        case BTN_60: SelectLast(60); break;
        case BTN_ALL: SelectLast(0); break;
        case BTN_PREVIEW: TogglePreview(); break;
        case BTN_SAVE: Save(); return;
        case BTN_CANCEL: DestroyWindow(g->hwnd); return;
        default: return;
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

// Called 20 times a second: pulls in new audio and scrolls the timeline.
void Tick() {
    if (g->playing &&
        static_cast<int64_t>(GetTickCount64() - g->playStart) * SampleRate() / 1000 >= g->playLen) {
        g->playing = false;
    }
    if (IsIconic(g->hwnd)) return;

    const int64_t newEnd = AlignDown(NowFrame() - TailFrames());
    if (newEnd <= g->end) return;
    const int64_t from = std::max(g->end, newEnd - g->windowFrames);
    bool added = Scan(from, newEnd);
    g->end = newEnd;
    TrimPeaks();

    // The selection stays pinned to the audio it covers; it only shrinks if that
    // audio scrolls out of the buffer.
    g->selA = std::max(g->selA, ViewStart());
    g->selB = std::max(g->selB, g->selA);

    if (added) {
        FitHeight();
        Layout();
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

// ---- Input ------------------------------------------------------------------

ButtonId ButtonAt(POINT pt) {
    for (const Button& b : g->buttons) {
        if (PtInRect(&b.rc, pt)) return b.id;
    }
    return BTN_NONE;
}

bool InTimeline(POINT pt) {
    return pt.x >= g->wave.left - S(6) && pt.x <= g->wave.right + S(6) && pt.y >= g->ruler.top &&
           pt.y < g->wave.bottom;
}

Drag EdgeAt(POINT pt) {
    if (!InTimeline(pt)) return Drag::None;
    if (std::abs(pt.x - XOf(g->selA)) <= S(6)) return Drag::EdgeA;
    if (std::abs(pt.x - XOf(g->selB)) <= S(6)) return Drag::EdgeB;
    return Drag::None;
}

void OnMouseDown(POINT pt) {
    SetCapture(g->hwnd);
    g->pressed = ButtonAt(pt);
    if (g->pressed != BTN_NONE) return;

    for (Lane& lane : g->lanes) {
        if (PtInRect(&lane.rc, pt) && pt.x < g->wave.left) {
            lane.on = !lane.on;
            InvalidateRect(g->hwnd, nullptr, FALSE);
            return;
        }
    }
    if (!InTimeline(pt)) return;
    StopPreview();
    g->drag = EdgeAt(pt);
    if (g->drag == Drag::None) {
        g->drag = Drag::New;
        g->prevA = g->selA;
        g->prevB = g->selB;
        g->anchor = FrameAt(pt.x);
        g->selA = g->selB = g->anchor;
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

void OnMouseMove(POINT pt) {
    if (g->drag == Drag::None) {
        ButtonId hover = ButtonAt(pt);
        if (hover != g->hover) {
            g->hover = hover;
            InvalidateRect(g->hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g->hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return;
    }
    int64_t f = FrameAt(pt.x);
    switch (g->drag) {
        case Drag::New:
            g->selA = std::min(g->anchor, f);
            g->selB = std::max(g->anchor, f);
            break;
        case Drag::EdgeA:
            g->selA = f;
            if (g->selA > g->selB) {
                std::swap(g->selA, g->selB);
                g->drag = Drag::EdgeB;
            }
            break;
        case Drag::EdgeB:
            g->selB = f;
            if (g->selB < g->selA) {
                std::swap(g->selA, g->selB);
                g->drag = Drag::EdgeA;
            }
            break;
        default: break;
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

void OnMouseUp(POINT pt) {
    ReleaseCapture();
    if (g->drag == Drag::New && g->selB - g->selA < SampleRate() / 100) {
        g->selA = g->prevA;  // a plain click keeps the old selection
        g->selB = g->prevB;
    }
    g->drag = Drag::None;
    ButtonId pressed = g->pressed;
    g->pressed = BTN_NONE;
    InvalidateRect(g->hwnd, nullptr, FALSE);
    if (pressed != BTN_NONE && ButtonAt(pt) == pressed) Activate(pressed);
}

bool OnSetCursor() {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g->hwnd, &pt);
    LPCWSTR cursor = IDC_ARROW;
    if (g->drag == Drag::EdgeA || g->drag == Drag::EdgeB || EdgeAt(pt) != Drag::None) {
        cursor = IDC_SIZEWE;
    } else if (g->drag == Drag::New || InTimeline(pt)) {
        cursor = IDC_IBEAM;
    } else if (ButtonAt(pt) != BTN_NONE) {
        cursor = IDC_HAND;
    } else {
        for (const Lane& lane : g->lanes) {
            if (PtInRect(&lane.rc, pt) && pt.x < g->wave.left) cursor = IDC_HAND;
        }
    }
    SetCursor(LoadCursorW(nullptr, cursor));
    return true;
}

void OnKey(WPARAM key) {
    switch (key) {
        case VK_RETURN: Save(); return;
        case VK_ESCAPE: DestroyWindow(g->hwnd); return;
        case VK_SPACE: TogglePreview(); break;
        case '1': SelectLast(5); break;
        case '2': SelectLast(10); break;
        case '3': SelectLast(30); break;
        case '4': SelectLast(60); break;
        case '5': SelectLast(0); break;
        case 'A': {
            bool any = CountOn() > 0;
            for (Lane& lane : g->lanes) lane.on = !any;
            break;
        }
        default: return;
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g) return DefWindowProcW(hwnd, msg, wParam, lParam);
    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    switch (msg) {
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize = {S(620), S(240)};
            return 0;
        }
        case WM_DPICHANGED: {
            g->dpi = HIWORD(wParam);
            MakeFonts();
            auto* r = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            if (!g->backDc || g->backSize.cx != client.right || g->backSize.cy != client.bottom) {
                if (g->backDc) DeleteDC(g->backDc);
                if (g->backBmp) DeleteObject(g->backBmp);
                g->backDc = CreateCompatibleDC(dc);
                g->backBmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
                SelectObject(g->backDc, g->backBmp);
                g->backSize = {client.right, client.bottom};
            }
            Paint(g->backDc, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, g->backDc, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: OnMouseDown(pt); return 0;
        case WM_MOUSEMOVE: OnMouseMove(pt); return 0;
        case WM_LBUTTONUP: OnMouseUp(pt); return 0;
        case WM_MOUSELEAVE:
            g->hover = BTN_NONE;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && OnSetCursor()) return TRUE;
            break;
        case WM_KEYDOWN: OnKey(wParam); return 0;
        case WM_TIMER:
            if (wParam == kLiveTimer) Tick();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kLiveTimer);
            StopPreview();
            return 0;
        case WM_NCDESTROY:
            for (HFONT f : {g->font, g->bold, g->tiny}) {
                if (f) DeleteObject(f);
            }
            if (g->backDc) DeleteDC(g->backDc);
            if (g->backBmp) DeleteObject(g->backBmp);
            delete g;
            g = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool PickerOpen(HINSTANCE instance, int64_t defaultFrames) {
    if (g) {
        PickerFocus();
        return true;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = MakeAppIcon(32);
        wc.hIconSm = MakeAppIcon(16);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g = new State();
    g->windowFrames = AlignDown(static_cast<int64_t>(g_settings.bufferSeconds) * SampleRate());
    g->end = AlignDown(NowFrame() - TailFrames());
    Scan(WindowStart(), g->end);
    if (g->lanes.empty()) {
        delete g;
        g = nullptr;
        return false;
    }
    g->selB = g->end;
    g->selA = std::max(ViewStart(), g->end - defaultFrames);

    POINT cursor;
    GetCursorPos(&cursor);
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX;
    g->hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Ephemeront", style, cursor.x, cursor.y, 100, 100, nullptr,
                              nullptr, instance, nullptr);
    if (!g->hwnd) {
        delete g;
        g = nullptr;
        return false;
    }
    g->dpi = GetDpiForWindow(g->hwnd);
    MakeFonts();
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    RECT rc = {0, 0, S(820), DesiredClientHeight()};
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_TOPMOST, g->dpi);
    int w = std::min<int>(rc.right - rc.left, work.right - work.left);
    int h = std::min<int>(rc.bottom - rc.top, (work.bottom - work.top) * 85 / 100);
    SetWindowPos(g->hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
                 work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);
    SetTimer(g->hwnd, kLiveTimer, kLiveMs, nullptr);
    ShowWindow(g->hwnd, SW_SHOW);
    SetForegroundWindow(g->hwnd);
    return true;
}

bool PickerIsOpen() { return g != nullptr; }

void PickerClose() {
    if (g) DestroyWindow(g->hwnd);
}

void PickerFocus() {
    if (!g) return;
    ShowWindow(g->hwnd, SW_RESTORE);
    SetForegroundWindow(g->hwnd);
}
