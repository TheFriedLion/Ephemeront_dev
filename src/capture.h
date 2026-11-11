// Audio capture: one rolling buffer per source (each app, the full system mix,
// and the microphone), managed on a background thread.
#pragma once

#include "common.h"

namespace Capture {

struct Config {
    bool apps = true;
    bool system = true;
    bool mic = false;
    int micMono = 0;  // 0 = stereo, 1 = mono from input 1 (left), 2 = mono from input 2 (right)
    int bufferSeconds = 120;
};

struct SourceInfo {
    uint64_t id;
    std::wstring name;
    int64_t lastSoundFrame;  // INT64_MIN if it has never made sound
};

// The default output device's native sample rate (what Windows mixes at).
int DeviceMixRate();

void Start(const Config& config);
void Stop();

// Takes effect within a moment. Changing bufferSeconds clears all buffers.
void Configure(const Config& config);

// Current sources: system mix first, then the mic, then apps by name.
std::vector<SourceInfo> Sources();

// Copies [start, start + frames) of one source into out (resized to fit).
// Returns false if that source no longer exists.
bool Read(uint64_t id, int64_t start, int64_t frames, std::vector<float>& out);

// Copies [start, start + frames) from every source, in Sources() order.
// With dropSilent, sources that were silent over that range are left out.
std::vector<Clip> Grab(int64_t start, int64_t frames, bool dropSilent);

void Stats(int* sources, size_t* bytes);

}  // namespace Capture
