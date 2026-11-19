#include "picker.h"

#include "app.h"
#include "capture.h"
#include "settings.h"
#include "wav.h"

#include <dwmapi.h>
#include <mmsystem.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kClass[] = L"Ephemeront.Picker";
constexpr UINT_PTR kLiveTimer = 1;
constexpr UINT kLiveMs = 50;  // 20 updates a second, only while the window is open

// Waveform summary: one min/max peak per kBlock frames, plus a coarser level (one
// per kCoarse blocks) for zoomed-out views. Kept in chunks keyed by clock
// position, and only where there was sound.
constexpr int64_t kBlock = 64;
constexpr int64_t kCoarse = 32;
constexpr int64_t kChunkBlocks = 2048;  // about 3 seconds per chunk
constexpr int64_t kChunkFrames = kBlock * kChunkBlocks;

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
constexpr COLORREF kTrack = RGB(38, 38, 46);
constexpr COLORREF kThumb = RGB(74, 74, 86);
constexpr COLORREF kThumbHot = RGB(98, 98, 112);
constexpr COLORREF kSplice = RGB(110, 110, 122);
constexpr COLORREF kLaneColors[] = {RGB(242, 169, 59),  RGB(96, 178, 255),  RGB(120, 214, 150), RGB(236, 112, 140),
                                    RGB(178, 140, 255), RGB(90, 210, 210),  RGB(240, 220, 110), RGB(255, 140, 90)};

enum ButtonId { BTN_NONE, BTN_LIVE, BTN_NOW, BTN_5, BTN_10, BTN_30, BTN_60, BTN_ALL, BTN_PREVIEW, BTN_SAVE, BTN_CANCEL };

struct Button {
    ButtonId id;
    const wchar_t* label;
    RECT rc;
};

struct Peak {
    float lo, hi;
};

struct PeakChunk {
    Peak fine[kChunkBlocks];
    Peak coarse[kChunkBlocks / kCoarse];
};

struct Lane {
    Lane() = default;
    Lane(Lane&&) = default;
    Lane& operator=(Lane&&) = default;
    Lane(const Lane&) = delete;  // so the lane list moves lanes instead of copying them
    Lane& operator=(const Lane&) = delete;

    uint64_t id = 0;
    std::wstring name;
    bool on = true;
    COLORREF color = 0;
    std::map<int64_t, std::unique_ptr<PeakChunk>> chunks;  // by clock frame / kChunkFrames
    RECT rc{}, check{};
};

struct Range {
    int64_t a, b;
};

// The part of the recording one pixel column of the waveform shows.
struct ColPiece {
    int col;
    int64_t r0, r1;  // clock frames
};

enum class Drag { None, New, EdgeA, EdgeB, Pan, Thumb };

struct State {
    HWND hwnd = nullptr;
    std::vector<Lane> lanes;
    Capture::Timeline tl;        // as of the last update
    std::vector<Range> covered;  // clock ranges already summarized, sorted
    bool liveView = true;        // the Live toggle: keep pulling in new audio
    bool follow = true;          // the view sticks to the newest audio
    double zoom = 1;             // frames per pixel
    int64_t viewStart = 0;       // tape position at the left edge
    int64_t selA = 0, selB = 0;  // selection, in tape positions
    int colors = 0;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr, tiny = nullptr;
    RECT header{}, scroll{}, ruler{}, wave{};
    std::vector<Button> buttons;
    ButtonId hover = BTN_NONE, pressed = BTN_NONE;
    bool hoverThumb = false;
    Drag drag = Drag::None;
    int64_t anchor = 0, prevA = 0, prevB = 0;
    int dragX = 0;
    int64_t dragView = 0;
    double dragScale = 1;
    int quietShown = -1;
    std::vector<uint8_t> previewWav;
    bool playing = false;
    ULONGLONG playStart = 0;
    int64_t playFrom = 0, playLen = 0;
    std::vector<float> scratch;
    std::vector<float> colTop, colBottom;
    std::vector<char> colHas;
    std::vector<ColPiece> cols;
    std::vector<POINT> segPoints[2];  // waveform columns, batched by color
    HDC backDc = nullptr;             // off-screen image, reused between frames
    HBITMAP backBmp = nullptr;
    SIZE backSize = {};
};

State* g = nullptr;

int S(int v) { return MulDiv(v, g->dpi, 96); }

int64_t FloorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int64_t CeilDiv(int64_t a, int64_t b) { return -FloorDiv(-a, b); }
int64_t AlignDown(int64_t f) { return FloorDiv(f, kBlock) * kBlock; }

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

int TextWidth(HDC dc, HFONT font, const wchar_t* text) {
    SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    return size.cx;
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

// "4.2 s", "32 s", "2:05", "1:02:05"
std::wstring Duration(double seconds) {
    wchar_t s[32];
    const int whole = static_cast<int>(seconds + 0.5);
    if (seconds < 9.95) swprintf_s(s, L"%.1f s", seconds);
    else if (whole < 60) swprintf_s(s, L"%d s", whole);
    else if (whole < 3600) swprintf_s(s, L"%d:%02d", whole / 60, whole % 60);
    else swprintf_s(s, L"%d:%02d:%02d", whole / 3600, whole / 60 % 60, whole % 60);
    return s;
}

// "12s", "3m 05s", "1h 05m"
std::wstring Gap(double seconds) {
    wchar_t s[32];
    const int whole = static_cast<int>(seconds + 0.5);
    if (whole < 1) wcscpy_s(s, L"<1s");
    else if (whole < 60) swprintf_s(s, L"%ds", whole);
    else if (whole < 3600) swprintf_s(s, L"%dm %02ds", whole / 60, whole % 60);
    else swprintf_s(s, L"%dh %02dm", whole / 3600, whole / 60 % 60);
    return s;
}

// ---- View -------------------------------------------------------------------
// The view shows tape positions (recorded audio only, cuts spliced shut) at a
// fixed zoom. While following, it scrolls with the newest audio; once you zoom,
// pan or start selecting, it holds still until you return to the end.

int WaveWidth() { return std::max<int>(1, g->wave.right - g->wave.left); }
int64_t Fpp() { return std::max<int64_t>(1, std::llround(g->zoom)); }
int64_t ViewLen() { return Fpp() * WaveWidth(); }
int64_t ViewEnd() { return g->viewStart + ViewLen(); }
int64_t MinFpp() { return std::max<int64_t>(8, SampleRate() / 1400); }
int64_t MaxFpp() {
    const int64_t content = std::max<int64_t>(static_cast<int64_t>(g_settings.bufferSeconds) * SampleRate(),
                                              g->tl.Frames());
    return std::max(MinFpp(), CeilDiv(content, WaveWidth()));
}

int64_t FollowStart() { return CeilDiv(g->tl.TapeEnd(), Fpp()) * Fpp() - ViewLen(); }

void SyncFollow() {
    if (g->follow) g->viewStart = FollowStart();
}

// After the user moves the view: snap to whole pixels, keep it on the
// recording, and pick following back up if it reaches the end.
void ClampView() {
    const int64_t fpp = Fpp();
    const int64_t last = FollowStart();
    g->viewStart = FloorDiv(g->viewStart, fpp) * fpp;
    if (g->viewStart >= last) {
        g->follow = true;
        g->viewStart = last;
        return;
    }
    g->follow = false;
    const int64_t first = FloorDiv(g->tl.TapeStart(), fpp) * fpp;
    g->viewStart = std::max(g->viewStart, std::min(first, last));
}

int XOf(int64_t tape) {
    const int64_t px = FloorDiv(tape - g->viewStart, Fpp());
    return g->wave.left + static_cast<int>(std::clamp<int64_t>(px, -100000, 100000));
}

int64_t FrameAt(int x) {
    const int64_t f = g->viewStart + static_cast<int64_t>(x - g->wave.left) * Fpp();
    return std::clamp(f, g->tl.TapeStart(), g->tl.TapeEnd());
}

void ZoomAt(int x, double factor) {
    const int col = std::clamp<int>(x - g->wave.left, 0, WaveWidth());
    const int64_t anchor = g->viewStart + col * Fpp();
    g->zoom = std::clamp(g->zoom * factor, static_cast<double>(MinFpp()), static_cast<double>(MaxFpp()));
    g->viewStart = anchor - col * Fpp();
    ClampView();
}

// Moves the view by dx pixels; positive moves toward older audio.
void Pan(int dx) {
    g->viewStart -= static_cast<int64_t>(dx) * Fpp();
    ClampView();
}

struct ScrollGeom {
    int64_t cs, ce;  // tape range the track stands for
    RECT thumb;
};

ScrollGeom Scroll() {
    ScrollGeom s;
    s.cs = std::min(g->tl.TapeStart(), g->viewStart);
    s.ce = std::max({g->tl.TapeEnd(), ViewEnd(), s.cs + 1});
    const RECT& r = g->scroll;
    const double scale = (r.right - r.left) / static_cast<double>(s.ce - s.cs);
    int l = r.left + static_cast<int>((g->viewStart - s.cs) * scale);
    int rt = r.left + static_cast<int>((ViewEnd() - s.cs) * scale);
    const int minW = S(18);
    if (rt - l < minW) {
        l = std::clamp<int>((l + rt) / 2 - minW / 2, r.left, std::max<int>(r.left, r.right - minW));
        rt = l + minW;
    }
    s.thumb = {l, r.top, rt, r.bottom};
    return s;
}

int ScrollX(const ScrollGeom& s, int64_t tape) {
    const RECT& r = g->scroll;
    const double t = std::clamp((tape - s.cs) / static_cast<double>(s.ce - s.cs), 0.0, 1.0);
    return r.left + static_cast<int>(t * (r.right - r.left));
}

// ---- Waveform summary ---------------------------------------------------------

// Summarizes clock frames [start, start + frames) into lane peaks. Returns true if
// any of it was loud.
bool AppendPeaks(Lane& lane, const float* pcm, int64_t start, int64_t frames) {
    bool loud = false;
    const int64_t first = start / kBlock;
    for (int64_t b = 0; b < frames / kBlock; ++b) {
        const float* p = pcm + b * kBlock * kChannels;
        float lo = 0, hi = 0;
        for (int64_t i = 0; i < kBlock * kChannels; ++i) {
            lo = std::min(lo, p[i]);
            hi = std::max(hi, p[i]);
        }
        if (lo == 0 && hi == 0) continue;
        const int64_t block = first + b;
        std::unique_ptr<PeakChunk>& chunk = lane.chunks[block / kChunkBlocks];
        if (!chunk) chunk = std::make_unique<PeakChunk>();
        const int64_t i = block % kChunkBlocks;
        chunk->fine[i] = {lo, hi};
        Peak& c = chunk->coarse[i / kCoarse];
        c.lo = std::min(c.lo, lo);
        c.hi = std::max(c.hi, hi);
        if (hi > kSilentPeak || lo < -kSilentPeak) loud = true;
    }
    return loud;
}

// Min and max over clock frames [r0, r1), at fine or coarse resolution.
void Accumulate(const Lane& lane, int64_t r0, int64_t r1, bool coarse, float& lo, float& hi) {
    const int64_t unit = coarse ? kBlock * kCoarse : kBlock;
    const int64_t perChunk = coarse ? kChunkBlocks / kCoarse : kChunkBlocks;
    const int64_t b0 = FloorDiv(r0, unit), b1 = std::max(b0 + 1, CeilDiv(r1, unit));
    for (int64_t b = b0; b < b1;) {
        const int64_t ci = FloorDiv(b, perChunk);
        const int64_t stop = std::min(b1, (ci + 1) * perChunk);
        auto it = lane.chunks.find(ci);
        if (it != lane.chunks.end()) {
            const Peak* peaks = coarse ? it->second->coarse : it->second->fine;
            for (int64_t k = b; k < stop; ++k) {
                const Peak& p = peaks[k - ci * perChunk];
                lo = std::min(lo, p.lo);
                hi = std::max(hi, p.hi);
            }
        }
        b = stop;
    }
}

// Pulls clock frames [from, to) from every source. Sources that make sound for
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
            const int64_t n = std::min(piece, to - pos);
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

// Summarizes every recorded stretch not seen yet. Returns true if a lane was added.
bool Summarize() {
    bool added = false;
    for (const Capture::Span& span : g->tl.spans) {
        const int64_t a = AlignDown(span.real), b = AlignDown(span.RealEnd());
        // The parts of [a, b) not covered yet.
        std::vector<Range> todo;
        int64_t pos = a;
        for (const Range& c : g->covered) {
            if (c.b <= pos) continue;
            if (c.a >= b) break;
            if (c.a > pos) todo.push_back({pos, c.a});
            pos = std::max(pos, c.b);
        }
        if (pos < b) todo.push_back({pos, b});

        for (const Range& r : todo) {
            added |= Scan(r.a, r.b);
            g->covered.push_back(r);
        }
        if (!todo.empty()) {
            std::sort(g->covered.begin(), g->covered.end(), [](const Range& x, const Range& y) { return x.a < y.a; });
            std::vector<Range> merged;
            for (const Range& r : g->covered) {
                if (!merged.empty() && r.a <= merged.back().b) merged.back().b = std::max(merged.back().b, r.b);
                else merged.push_back(r);
            }
            g->covered.swap(merged);
        }
    }
    return added;
}

// Forgets summaries of audio that has left the buffer.
void Forget() {
    if (g->tl.Empty()) return;
    const int64_t oldest = g->tl.spans.front().real;
    for (Lane& lane : g->lanes) {
        while (!lane.chunks.empty() && (lane.chunks.begin()->first + 1) * kChunkFrames <= oldest) {
            lane.chunks.erase(lane.chunks.begin());
        }
    }
    while (!g->covered.empty() && g->covered.front().b <= oldest) g->covered.erase(g->covered.begin());
}

// ---- Layout and painting ----------------------------------------------------

int DesiredClientHeight() { return S(146 + static_cast<int>(g->lanes.size()) * 62); }

const wchar_t* LiveLabel() {
    if (!g->liveView) return L"Frozen";
    return g->tl.live ? L"Live" : L"Live · quiet";
}

void Layout() {
    RECT c;
    GetClientRect(g->hwnd, &c);
    const int m = S(14), nameW = S(160), gap = S(6), footerH = S(32);
    g->header = {m, m, c.right - m, m + S(24)};
    g->scroll = {m + nameW, g->header.bottom + S(10), c.right - m, g->header.bottom + S(22)};
    g->ruler = {m + nameW, g->scroll.bottom + S(6), c.right - m, g->scroll.bottom + S(22)};

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

    // The Live toggle sits after the title, sized for its longest label.
    HDC dc = GetDC(g->hwnd);
    const int titleW = TextWidth(dc, g->bold, L"Clip Picker");
    const int liveW = TextWidth(dc, g->font, L"Live · quiet");
    ReleaseDC(g->hwnd, dc);
    const int liveX = g->header.left + titleW + S(14);
    g->buttons.push_back({BTN_LIVE, L"", {liveX, g->header.top, liveX + S(36) + liveW, g->header.bottom}});

    const int scrollMid = (g->scroll.top + g->scroll.bottom) / 2;
    g->buttons.push_back({BTN_NOW, L"Now ▸", {g->wave.left - S(62), scrollMid - S(11), g->wave.left - S(8), scrollMid + S(11)}});

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

bool ButtonShown(ButtonId id) { return id != BTN_NOW || !g->follow; }

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

// Works out, once per frame, which clock ranges each pixel column shows.
void BuildColumns() {
    g->cols.clear();
    const int w = WaveWidth();
    const int64_t fpp = Fpp();
    const auto& spans = g->tl.spans;
    size_t s = 0;
    for (int col = 0; col < w; ++col) {
        const int64_t ta = g->viewStart + col * fpp, tb = ta + fpp;
        while (s < spans.size() && spans[s].TapeEnd() <= ta) ++s;
        for (size_t k = s; k < spans.size() && spans[k].tape < tb; ++k) {
            const int64_t a = std::max(ta, spans[k].tape), b = std::min(tb, spans[k].TapeEnd());
            if (a < b) g->cols.push_back({col, spans[k].real + (a - spans[k].tape), spans[k].real + (b - spans[k].tape)});
        }
    }
}

bool Overlaps(const RECT& rc, const std::vector<RECT>& taken) {
    for (const RECT& t : taken) {
        if (rc.left < t.right && rc.right > t.left) return true;
    }
    return false;
}

// Time marks count back from the newest audio, in recorded time.
void DrawRuler(HDC dc, std::vector<RECT>& taken) {
    const double rate = SampleRate();
    const int64_t end = g->tl.TapeEnd();

    // Where silence was cut, a marker says how much.
    const auto& spans = g->tl.spans;
    for (size_t i = 1; i < spans.size(); ++i) {
        const int x = XOf(spans[i].tape);
        if (x < g->wave.left || x >= g->wave.right) continue;
        const std::wstring label = Gap((spans[i].real - spans[i - 1].RealEnd()) / rate) + L" skipped";
        const int w = TextWidth(dc, g->tiny, label.c_str());
        RECT rc = {x + S(4), g->ruler.top, x + S(4) + w, g->ruler.bottom - S(2)};
        Fill(dc, {x, g->ruler.top + S(2), x + 1, g->ruler.bottom}, kSplice);
        if (rc.right > g->wave.right || Overlaps(rc, taken)) continue;
        Text(dc, g->tiny, kSplice, label.c_str(), rc, DT_LEFT | DT_VCENTER);
        taken.push_back(rc);
    }

    // While everything is quiet, the end of the recording says for how long.
    const int xEnd = XOf(end);
    if (!g->tl.live && xEnd >= g->wave.left && xEnd <= g->wave.right) {
        const std::wstring label = L"quiet " + Gap((NowFrame() - g->tl.lastSound) / rate);
        const int w = TextWidth(dc, g->tiny, label.c_str());
        RECT rc = {xEnd - S(3) - w, g->ruler.top, xEnd - S(3), g->ruler.bottom - S(2)};
        if (!Overlaps(rc, taken)) {
            Text(dc, g->tiny, kMuted, label.c_str(), rc, DT_RIGHT | DT_VCENTER);
            taken.push_back(rc);
        }
    }

    const double span = ViewLen() / rate;
    const double steps[] = {0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double step = steps[_countof(steps) - 1];
    for (double s : steps) {
        if (span / s <= 8) {
            step = s;
            break;
        }
    }
    const int64_t stepF = std::max<int64_t>(1, std::llround(step * rate));
    const int64_t k0 = std::max<int64_t>(0, FloorDiv(end - ViewEnd(), stepF));
    const int64_t k1 = FloorDiv(end - g->viewStart, stepF);
    for (int64_t k = k0; k <= k1 && k - k0 < 40; ++k) {
        const int x = XOf(end - k * stepF);
        if (x < g->wave.left || x > g->wave.right) continue;
        Fill(dc, {x, g->ruler.bottom - S(4), x + 1, g->ruler.bottom}, kMuted);
        wchar_t label[24] = {};
        const double secs = k * step;
        if (k == 0) {
            if (g->tl.live) wcscpy_s(label, L"now");
        } else if (step < 1) {
            swprintf_s(label, L"-%.2f", secs);
            wchar_t* p = label + wcslen(label) - 1;  // 0.50 -> 0.5
            while (*p == L'0') *p-- = 0;
            if (*p == L'.') *p = 0;
            wcscat_s(label, L"s");
        } else if (secs >= 60 && std::fmod(secs, 60) == 0) {
            swprintf_s(label, L"-%dm", static_cast<int>(secs / 60));
        } else if (secs >= 60) {
            swprintf_s(label, L"-%d:%02d", static_cast<int>(secs) / 60, static_cast<int>(secs) % 60);
        } else {
            swprintf_s(label, L"-%ds", static_cast<int>(secs));
        }
        if (!label[0]) continue;
        RECT rc = {x - S(46), g->ruler.top, x - S(3), g->ruler.bottom - S(2)};
        if (rc.left < g->ruler.left - S(10) || Overlaps(rc, taken)) continue;
        Text(dc, g->tiny, kMuted, label, rc, DT_RIGHT | DT_VCENTER);
    }
}

void DrawLane(HDC dc, const Lane& lane, int xa, int xb) {
    Fill(dc, lane.rc, kLaneBg);
    if (xb > g->wave.left && xa < g->wave.right) {
        Fill(dc, {std::max<int>(xa, g->wave.left), lane.rc.top, std::min<int>(xb, g->wave.right), lane.rc.bottom},
             kSelBg);
    }

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
    const bool coarse = Fpp() >= 4 * kBlock * kCoarse;
    g->colTop.assign(w, 0.0f);
    g->colBottom.assign(w, 0.0f);
    g->colHas.assign(w, 0);
    float norm = 0.1f;
    for (const ColPiece& c : g->cols) {
        float lo = 0, hi = 0;
        Accumulate(lane, c.r0, c.r1, coarse, lo, hi);
        g->colTop[c.col] = std::max(g->colTop[c.col], hi);
        g->colBottom[c.col] = std::min(g->colBottom[c.col], lo);
        g->colHas[c.col] = 1;
        norm = std::max(norm, std::max(hi, -lo));
    }

    // Each column is a short vertical line. Lines are grouped by color and drawn in
    // one call per color, which keeps live redraws cheap. Columns outside the
    // recording are left empty.
    const int mid = (lane.rc.top + lane.rc.bottom) / 2;
    const int half = std::max<int>(1, (lane.rc.bottom - lane.rc.top) / 2 - S(4));
    const COLORREF colors[2] = {lane.on ? lane.color : kOffWave, lane.on ? Dim(lane.color) : kOffWave};
    for (auto& pts : g->segPoints) pts.clear();
    for (int col = 0; col < w; ++col) {
        if (!g->colHas[col]) continue;
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

void DrawScrollRow(HDC dc) {
    // View length and recording length, left of the bar.
    const double rate = SampleRate();
    const std::wstring info = Duration(ViewLen() / rate) + L" of " + Duration(g->tl.Frames() / rate);
    RECT text = {g->header.left, g->scroll.top - S(5), g->wave.left - S(8), g->scroll.bottom + S(5)};
    if (!g->follow) text.right = g->wave.left - S(68);
    Text(dc, g->tiny, kMuted, info.c_str(), text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    const ScrollGeom s = Scroll();
    FillRound(dc, g->scroll, kTrack, S(6));
    for (size_t i = 1; i < g->tl.spans.size(); ++i) {  // cuts
        const int x = ScrollX(s, g->tl.spans[i].tape);
        Fill(dc, {x, g->scroll.top + S(2), x + 1, g->scroll.bottom - S(2)}, kSplice);
    }
    const bool hot = g->hoverThumb || g->drag == Drag::Thumb;
    FillRound(dc, s.thumb, hot ? kThumbHot : kThumb, S(6));
    if (g->selB > g->selA) {  // where the selection is
        int a = ScrollX(s, g->selA), b = std::max(ScrollX(s, g->selB), a + std::max(2, S(2)));
        Fill(dc, {a, g->scroll.bottom - S(3), b, g->scroll.bottom}, kAccent);
    }
}

void Paint(HDC dc, const RECT& client) {
    Fill(dc, client, kBg);
    SetBkMode(dc, TRANSPARENT);

    Text(dc, g->bold, kText, L"Clip Picker", g->header, DT_LEFT | DT_VCENTER);
    wchar_t info[96];
    swprintf_s(info, L"%s selected  ·  %d of %d tracks", Duration((g->selB - g->selA) / double(SampleRate())).c_str(),
               CountOn(), static_cast<int>(g->lanes.size()));
    Text(dc, g->font, kMuted, info, g->header, DT_RIGHT | DT_VCENTER);

    DrawScrollRow(dc);

    BuildColumns();
    const int xa = XOf(g->selA);
    const int xb = std::max(XOf(g->selB), xa + 1);
    for (const Lane& lane : g->lanes) DrawLane(dc, lane, xa, xb);

    // Dashed lines through the tracks where silence was cut.
    for (size_t i = 1; i < g->tl.spans.size(); ++i) {
        const int x = XOf(g->tl.spans[i].tape);
        if (x < g->wave.left || x >= g->wave.right) continue;
        for (int y = g->wave.top; y < g->wave.bottom; y += S(6)) {
            Fill(dc, {x, y, x + 1, std::min<int>(y + S(3), g->wave.bottom)}, kSplice);
        }
    }

    std::vector<RECT> taken;
    DrawRuler(dc, taken);

    const int w = std::max(2, S(2));
    auto edge = [&](int x) {
        if (x >= g->wave.left - w && x <= g->wave.right + w) {
            Fill(dc, {x - w / 2, g->ruler.top, x - w / 2 + w, g->wave.bottom}, kAccent);
        }
    };
    edge(xa);
    edge(xb);

    if (g->playing) {
        int64_t played = static_cast<int64_t>(GetTickCount64() - g->playStart) * SampleRate() / 1000;
        int x = XOf(g->playFrom + std::min(played, g->playLen));
        if (x >= g->wave.left && x <= g->wave.right) {
            Fill(dc, {x, g->wave.top, x + std::max(1, S(1)), g->wave.bottom}, kText);
        }
    }

    int hintLeft = 0, hintRight = client.right;
    for (const Button& b : g->buttons) {
        if (!ButtonShown(b.id)) continue;
        const bool hot = b.id == g->hover;
        if (b.id == BTN_LIVE) {
            FillRound(dc, b.rc, hot ? kButtonHover : kButton, S(10));
            const int midY = (b.rc.top + b.rc.bottom) / 2, dotX = b.rc.left + S(11);
            const COLORREF dot = !g->liveView ? kMuted : g->tl.live ? kLive : Dim(kLive);
            FillRound(dc, {dotX, midY - S(4), dotX + S(8), midY + S(4)}, dot, S(8));
            RECT label = {dotX + S(14), b.rc.top, b.rc.right - S(6), b.rc.bottom};
            Text(dc, g->font, g->liveView ? kText : kMuted, LiveLabel(), label, DT_LEFT | DT_VCENTER);
            if (!g->liveView) {
                RECT note = {b.rc.right + S(10), b.rc.top, b.rc.right + S(150), b.rc.bottom};
                Text(dc, g->tiny, kMuted, L"still recording", note, DT_LEFT | DT_VCENTER);
            }
            continue;
        }
        COLORREF bg = b.id == BTN_SAVE ? (hot ? kAccentHover : kAccent) : (hot ? kButtonHover : kButton);
        COLORREF fg = b.id == BTN_SAVE ? kBg : kText;
        const wchar_t* label = (b.id == BTN_PREVIEW && g->playing) ? L"■ Stop" : b.label;
        FillRound(dc, b.rc, bg, S(8));
        HFONT font = b.id == BTN_SAVE ? g->bold : b.id == BTN_NOW ? g->tiny : g->font;
        Text(dc, font, fg, label, b.rc, DT_CENTER | DT_VCENTER);
        if (b.id == BTN_ALL) hintLeft = b.rc.right + S(14);
        if (b.id == BTN_PREVIEW) hintRight = b.rc.left - S(14);
    }
    if (hintRight - hintLeft > S(120)) {
        RECT hint = {hintLeft, g->buttons.back().rc.top, hintRight, g->buttons.back().rc.bottom};
        Text(dc, g->tiny, kMuted, L"Drag to select  ·  Wheel zooms  ·  Middle-drag moves",
             hint, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
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
    const Capture::Timeline tl = Capture::GetTimeline();
    std::vector<float> mix(static_cast<size_t>(len) * kChannels, 0.0f);
    for (const Lane& lane : g->lanes) {
        if (!lane.on || !Capture::ReadTape(lane.id, tl, g->selA, len, g->scratch)) continue;
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
    const Capture::Timeline tl = Capture::GetTimeline();
    const int64_t from = std::max(g->selA, tl.TapeStart());
    const int64_t frames = g->selB - from;
    std::vector<Clip> clips;
    for (const Lane& lane : g->lanes) {
        if (!lane.on || frames <= 0) continue;
        Clip clip;
        clip.id = lane.id;
        clip.name = lane.name;
        if (Capture::ReadTape(lane.id, tl, from, frames, clip.pcm)) clips.push_back(std::move(clip));
    }
    if (clips.empty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    std::vector<const Clip*> chosen;
    for (const Clip& c : clips) chosen.push_back(&c);
    const std::vector<std::wstring> saved = SaveClips(chosen, 0, frames, g_settings.skipSilent);
    DestroyWindow(g->hwnd);  // frees g
    AppNotifySaved(saved, frames / double(SampleRate()));
}

// Selects the newest `seconds` of recording (0 = all of it) and shows it live.
void SelectLast(int seconds) {
    StopPreview();
    g->selB = g->tl.TapeEnd();
    g->selA = seconds > 0 ? std::max(g->tl.TapeStart(), g->selB - static_cast<int64_t>(seconds) * SampleRate())
                          : g->tl.TapeStart();
    const int64_t len = g->selB - g->selA;
    if (len * 21 / 20 > ViewLen()) {
        g->zoom = std::clamp(len * 1.08 / WaveWidth(), static_cast<double>(MinFpp()), static_cast<double>(MaxFpp()));
    }
    g->follow = true;
    SyncFollow();
}

// Pulls in the latest recording. Returns true if anything on screen changed.
bool Refresh() {
    Capture::Timeline tl = Capture::GetTimeline();
    bool changed = tl.TapeEnd() != g->tl.TapeEnd() || tl.TapeStart() != g->tl.TapeStart() ||
                   tl.spans.size() != g->tl.spans.size() || tl.live != g->tl.live;
    g->tl = std::move(tl);
    const bool added = Summarize();
    Forget();
    SyncFollow();
    // The selection stays on the audio it covers; it only shrinks if that audio
    // leaves the buffer.
    g->selA = std::clamp(g->selA, g->tl.TapeStart(), g->tl.TapeEnd());
    g->selB = std::clamp(g->selB, g->selA, g->tl.TapeEnd());
    if (added) {
        FitHeight();
        Layout();
        changed = true;
    }
    return changed;
}

void ToggleLive() {
    g->liveView = !g->liveView;
    if (g->liveView) Refresh();
}

void Activate(ButtonId id) {
    switch (id) {
        case BTN_LIVE: ToggleLive(); break;
        case BTN_NOW:
            g->follow = true;
            SyncFollow();
            break;
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

// Called 20 times a second. While everything is quiet nothing new is recorded,
// so nothing moves and nothing is redrawn.
void Tick() {
    bool dirty = false;
    if (g->playing) {
        if (static_cast<int64_t>(GetTickCount64() - g->playStart) * SampleRate() / 1000 >= g->playLen) {
            g->playing = false;
        }
        dirty = true;
    }
    if (IsIconic(g->hwnd)) return;
    if (g->liveView) {
        if (Refresh()) dirty = true;
        if (!g->tl.live) {
            const int quiet = static_cast<int>((NowFrame() - g->tl.lastSound) / SampleRate());
            if (quiet != g->quietShown) {
                g->quietShown = quiet;
                dirty = true;
            }
        }
    }
    if (dirty) InvalidateRect(g->hwnd, nullptr, FALSE);
}

// ---- Input ------------------------------------------------------------------

ButtonId ButtonAt(POINT pt) {
    for (const Button& b : g->buttons) {
        if (ButtonShown(b.id) && PtInRect(&b.rc, pt)) return b.id;
    }
    return BTN_NONE;
}

bool InTimeline(POINT pt) {
    return pt.x >= g->wave.left - S(6) && pt.x <= g->wave.right + S(6) && pt.y >= g->ruler.top &&
           pt.y < g->wave.bottom;
}

bool InScroll(POINT pt) {
    return pt.x >= g->scroll.left && pt.x < g->scroll.right && pt.y >= g->scroll.top - S(5) &&
           pt.y < g->scroll.bottom + S(4);
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
    if (InScroll(pt)) {
        ScrollGeom s = Scroll();
        if (!PtInRect(&s.thumb, pt)) {  // jump there, then drag from there
            const double scale = (s.ce - s.cs) / static_cast<double>(g->scroll.right - g->scroll.left);
            g->viewStart = s.cs + static_cast<int64_t>((pt.x - g->scroll.left) * scale) - ViewLen() / 2;
            ClampView();
            s = Scroll();
        }
        g->drag = Drag::Thumb;
        g->dragX = pt.x;
        g->dragView = g->viewStart;
        g->dragScale = (s.ce - s.cs) / static_cast<double>(g->scroll.right - g->scroll.left);
        InvalidateRect(g->hwnd, nullptr, FALSE);
        return;
    }
    if (!InTimeline(pt)) return;
    StopPreview();
    g->follow = false;  // hold the view still while selecting
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
        const RECT thumbRc = Scroll().thumb;
        bool thumb = InScroll(pt) && PtInRect(&thumbRc, pt);
        if (hover != g->hover || thumb != g->hoverThumb) {
            g->hover = hover;
            g->hoverThumb = thumb;
            InvalidateRect(g->hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g->hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return;
    }
    if (g->drag == Drag::Pan || g->drag == Drag::Thumb) {
        const int64_t moved = g->drag == Drag::Pan ? -static_cast<int64_t>(pt.x - g->dragX) * Fpp()
                                                   : std::llround((pt.x - g->dragX) * g->dragScale);
        g->viewStart = g->dragView + moved;
        ClampView();
        InvalidateRect(g->hwnd, nullptr, FALSE);
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

void OnMiddleDown(POINT pt) {
    if (!InTimeline(pt) && !InScroll(pt)) return;
    SetCapture(g->hwnd);
    g->drag = Drag::Pan;
    g->dragX = pt.x;
    g->dragView = g->viewStart;
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
}

void OnWheel(POINT screen, int delta, bool horizontal, bool shift) {
    POINT pt = screen;
    ScreenToClient(g->hwnd, &pt);
    if (horizontal || shift) {
        const int px = delta * WaveWidth() / (120 * 8);  // a notch moves an eighth of the view
        Pan(horizontal ? -px : px);
    } else {
        int x = pt.x;
        if (x < g->wave.left || x > g->wave.right) x = g->follow ? g->wave.right : (g->wave.left + g->wave.right) / 2;
        ZoomAt(x, std::pow(0.8, delta / 120.0));
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

bool OnSetCursor() {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g->hwnd, &pt);
    LPCWSTR cursor = IDC_ARROW;
    if (g->drag == Drag::Pan) {
        cursor = IDC_SIZEALL;
    } else if (g->drag == Drag::Thumb || (g->drag == Drag::None && InScroll(pt))) {
        cursor = IDC_ARROW;
    } else if (g->drag == Drag::EdgeA || g->drag == Drag::EdgeB || EdgeAt(pt) != Drag::None) {
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
    const int centerX = g->follow ? g->wave.right : (g->wave.left + g->wave.right) / 2;
    switch (key) {
        case VK_RETURN: Save(); return;
        case VK_ESCAPE: DestroyWindow(g->hwnd); return;
        case VK_SPACE: TogglePreview(); break;
        case '1': SelectLast(5); break;
        case '2': SelectLast(10); break;
        case '3': SelectLast(30); break;
        case '4': SelectLast(60); break;
        case '5': SelectLast(0); break;
        case 'L': ToggleLive(); break;
        case 'A': {
            bool any = CountOn() > 0;
            for (Lane& lane : g->lanes) lane.on = !any;
            break;
        }
        case VK_HOME:
            g->viewStart = g->tl.TapeStart();
            ClampView();
            break;
        case VK_END:
            g->follow = true;
            SyncFollow();
            break;
        case VK_LEFT: Pan(WaveWidth() / 8); break;
        case VK_RIGHT: Pan(-WaveWidth() / 8); break;
        case VK_OEM_PLUS:
        case VK_ADD: ZoomAt(centerX, 0.8); break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT: ZoomAt(centerX, 1.25); break;
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
            SyncFollow();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize = {S(620), S(260)};
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
        case WM_MBUTTONDOWN: OnMiddleDown(pt); return 0;
        case WM_MBUTTONUP:
            if (g->drag == Drag::Pan) {
                ReleaseCapture();
                g->drag = Drag::None;
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g->drag == Drag::Pan || g->drag == Drag::Thumb) g->drag = Drag::None;
            return 0;
        case WM_MOUSEWHEEL:
            OnWheel(pt, GET_WHEEL_DELTA_WPARAM(wParam), false, (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0);
            return 0;
        case WM_MOUSEHWHEEL:
            OnWheel(pt, GET_WHEEL_DELTA_WPARAM(wParam), true, false);
            return 0;
        case WM_MOUSELEAVE:
            g->hover = BTN_NONE;
            g->hoverThumb = false;
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
    g->tl = Capture::GetTimeline();
    if (!g->tl.Empty()) Summarize();
    if (g->lanes.empty()) {
        delete g;
        g = nullptr;
        return false;
    }
    g->selB = g->tl.TapeEnd();
    g->selA = std::max(g->tl.TapeStart(), g->selB - defaultFrames);

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

    // Start zoomed to show everything recorded (at least 10 seconds' worth). From
    // here new audio scrolls in from the right instead of squeezing the view.
    const int64_t shown = std::max<int64_t>(g->tl.Frames(), 10LL * SampleRate());
    g->zoom = std::clamp(static_cast<double>(shown) / WaveWidth(), static_cast<double>(MinFpp()),
                         static_cast<double>(MaxFpp()));
    g->follow = true;
    SyncFollow();

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
