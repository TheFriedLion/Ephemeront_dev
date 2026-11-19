// Audio capture: one buffer per source (each app, the full system mix, and the
// microphone), managed on a background thread.
//
// The buffer holds sound, not time. Whenever every source has been silent for
// more than a few seconds, recording is cut until something makes sound again,
// so a long quiet stretch never pushes older audio out. The result is a timeline
// of recorded spans; "tape" positions count recorded frames only, with each cut
// spliced shut.
#pragma once

#include "common.h"

namespace Capture {

// How much silence is kept after the last sound before recording is cut, and how
// much is kept before the first sound when it resumes.
constexpr int kKeepAfterSoundMs = 4000;
constexpr int kKeepBeforeSoundMs = 250;

struct Config {
    bool apps = true;
    bool system = true;
    bool mic = false;
    int micMono = 0;  // 0 = stereo, 1 = mono from input 1 (left), 2 = mono from input 2 (right)
    int bufferSeconds = 120;  // seconds of recorded tape to keep
};

struct SourceInfo {
    uint64_t id;
    std::wstring name;
    int64_t lastSoundFrame;  // INT64_MIN if it has never made sound
};

// One recorded stretch: clock frames [real, real + frames) sit at tape positions
// [tape, tape + frames). A tape position never changes once given out, even as
// old audio is dropped.
struct Span {
    int64_t real;
    int64_t tape;
    int64_t frames;
    int64_t RealEnd() const { return real + frames; }
    int64_t TapeEnd() const { return tape + frames; }
};

struct Piece {
    int64_t real;    // clock frame
    int64_t offset;  // frames from the start of the requested range
    int64_t frames;
};

struct Timeline {
    std::vector<Span> spans;  // oldest first; between two spans, everything was silent
    bool live = false;        // still recording (not cut for silence right now)
    int64_t lastSound = 0;    // clock frame of the most recent sound

    bool Empty() const { return spans.empty(); }
    int64_t TapeStart() const { return spans.empty() ? 0 : spans.front().tape; }
    int64_t TapeEnd() const { return spans.empty() ? 0 : spans.back().TapeEnd(); }
    int64_t Frames() const { return TapeEnd() - TapeStart(); }

    // The recorded clock ranges that make up tape [tape, tape + frames), in order.
    // Parts outside the recording are left out (they read as silence).
    std::vector<Piece> Pieces(int64_t tape, int64_t frames) const;
};

// The default output device's native sample rate (what Windows mixes at).
int DeviceMixRate();

void Start(const Config& config);
void Stop();

// Takes effect within a moment.
void Configure(const Config& config);

// Current sources: system mix first, then the mic, then apps by name.
std::vector<SourceInfo> Sources();

// The recording timeline as of now.
Timeline GetTimeline();

// Copies clock frames [start, start + frames) of one source into out (resized to
// fit). Returns false if that source no longer exists.
bool Read(uint64_t id, int64_t start, int64_t frames, std::vector<float>& out);

// Same, for tape [tape, tape + frames): the recorded pieces back to back.
bool ReadTape(uint64_t id, const Timeline& timeline, int64_t tape, int64_t frames, std::vector<float>& out);

// Copies tape [tape, tape + frames) from every source, in Sources() order.
// With dropSilent, sources that were silent over that range are left out.
std::vector<Clip> Grab(const Timeline& timeline, int64_t tape, int64_t frames, bool dropSilent);

void Stats(int* sources, size_t* bytes);

}  // namespace Capture
