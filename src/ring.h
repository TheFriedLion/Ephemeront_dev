// Rolling audio buffer for one source.
//
// Audio is stored in one-second chunks addressed by absolute frame number on the
// shared clock, so every source lines up in time. Chunks that are pure silence
// are never allocated, which keeps idle sources almost free.
#pragma once

#include "common.h"

class RingBuffer {
public:
    explicit RingBuffer(int seconds);
    ~RingBuffer();
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Stores a packet whose timestamp says it started at `frame`. pcm == nullptr
    // means silence. Packets are written back to back so audio stays continuous;
    // the timestamp is only used to realign (see ring.cpp). `lost` means the
    // stream reported a gap, so the packet goes exactly where its timestamp says.
    // Returns how far the write position jumped to realign, in frames (0 if none).
    int64_t Write(int64_t frame, const float* pcm, uint32_t frames, bool lost);

    // Copies [start, start + frames) into out. Anything not held reads as silence.
    void Read(int64_t start, int64_t frames, float* out) const;

    // Frame just after the most recent non-silent audio, or INT64_MIN if none yet.
    int64_t LastSoundFrame() const;
    size_t BytesInUse() const;

private:
    struct Chunk {
        int64_t index = -1;
        float* data = nullptr;
    };
    void FreeStale(int64_t currentChunk);

    const int64_t chunkFrames_;
    const size_t chunkBytes_;
    std::vector<Chunk> chunks_;
    int64_t writePos_ = -1;
    int64_t lastChunk_ = -1;
    int64_t lastSound_ = INT64_MIN;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
};
