// Audio store for one source.
//
// Audio is kept in one-second chunks addressed by absolute frame number on the
// shared clock, so every source lines up in time. Chunks that are pure silence
// are never allocated. Nothing ages out by itself: the capture layer decides
// which stretches of time are still worth keeping (see Prune), which lets a
// sound from an hour ago survive an hour of silence.
#pragma once

#include "common.h"

#include <map>

class RingBuffer {
public:
    RingBuffer();
    ~RingBuffer();
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    struct WriteResult {
        int64_t at;    // where the packet was actually placed
        int64_t jump;  // how far the write position jumped to realign (0 if none)
    };

    // Stores a packet whose timestamp says it started at `frame`. pcm == nullptr
    // means silence. Packets are written back to back so audio stays continuous;
    // the timestamp is only used to realign (see ring.cpp). `lost` means the
    // stream reported a gap, so the packet goes exactly where its timestamp says.
    WriteResult Write(int64_t frame, const float* pcm, uint32_t frames, bool lost);

    // Copies [start, start + frames) into out. Anything not held reads as silence.
    void Read(int64_t start, int64_t frames, float* out) const;

    // Frees every chunk that lies entirely before `before` and overlaps none of
    // the `keep` ranges ({start, end} pairs, sorted).
    void Prune(const std::vector<std::pair<int64_t, int64_t>>& keep, int64_t before);

    // Frame just after the most recent non-silent audio, or INT64_MIN if none yet.
    int64_t LastSoundFrame() const;
    size_t BytesInUse() const;

private:
    const int64_t chunkFrames_;
    const size_t chunkBytes_;
    std::map<int64_t, float*> chunks_;  // chunk index -> one second of audio
    int64_t writePos_ = -1;
    int64_t lastSound_ = INT64_MIN;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
};
