// Shared constants and small helpers used across Ephemeront.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Every source is stored in one internal format: 32-bit float stereo at the
// output device's native sample rate, which is how Windows mixes audio itself.
constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * static_cast<int>(sizeof(float));

// Fixed for the session; set once at startup from the default output device.
void SampleRateInit(int rate);
int SampleRate();

// Peak at or below this (-60 dBFS) counts as silence.
constexpr float kSilentPeak = 0.001f;

// How far behind "now" a save ends, so the newest audio has time to arrive.
inline int64_t TailFrames() { return SampleRate() * 30 / 1000; }

// Messages and IDs shared between windows.
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_COMMAND = WM_APP + 2;  // wParam = AppCommand
enum AppCommand : WPARAM { CMD_QUICK_SAVE = 1, CMD_PICKER, CMD_SETTINGS, CMD_EXIT };

constexpr wchar_t kMainWindowClass[] = L"Ephemeront.Main";

// A copy of part of one source's audio.
struct Clip {
    uint64_t id = 0;  // which source it came from
    std::wstring name;
    std::vector<float> pcm;  // interleaved stereo
    int64_t Frames() const { return static_cast<int64_t>(pcm.size()) / kChannels; }
};

// Shared sample clock: frames since the app started.
void ClockInit();
int64_t NowFrame();
int64_t QpcToFrame(uint64_t qpc100ns);

void LogInit(const std::wstring& path);
void Log(const wchar_t* fmt, ...);

std::wstring AppDataDir();       // %APPDATA%\Ephemeront
std::wstring DefaultClipsDir();  // Music\Ephemeront
std::wstring SanitizeFileName(const std::wstring& name);
std::wstring TimestampForFile();  // 2026-10-10 21-45-03

float PeakOf(const float* samples, size_t count);
HICON MakeAppIcon(int size);
