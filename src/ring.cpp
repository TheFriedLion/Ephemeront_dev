#include "ring.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

// Packet timestamps wobble by several milliseconds on real hardware (microphones
// especially), and device clocks drift slowly against the system clock. So packets
// are written back to back, and the write position only realigns with the
// timestamps when they disagree by more than kDriftFrames, and only during a quiet
// packet where the jump can't be heard. Past kForceFrames (a stream that paused
// during silence, or long-term drift with no quiet moment) it realigns regardless.
int64_t DriftFrames() { return SampleRate() / 50; }   // 20 ms
int64_t ForceFrames() { return SampleRate() / 4; }    // 250 ms
constexpr float kQuietPeak = 0.003f;                  // about -50 dBFS

bool IsSilent(const float* pcm, size_t samples) {
    for (size_t i = 0; i < samples; ++i) {
        if (pcm[i] > 1e-5f || pcm[i] < -1e-5f) return false;
    }
    return true;
}

}  // namespace

RingBuffer::RingBuffer(int seconds)
    : chunkFrames_(SampleRate()),
      chunkBytes_(static_cast<size_t>(SampleRate()) * kBytesPerFrame),
      chunks_(static_cast<size_t>(seconds) + 2) {}

RingBuffer::~RingBuffer() {
    for (Chunk& c : chunks_) free(c.data);
}

int64_t RingBuffer::Write(int64_t frame, const float* pcm, uint32_t frames, bool lost) {
    AcquireSRWLockExclusive(&lock_);

    int64_t jump = 0;
    if (writePos_ >= 0) {
        const int64_t drift = frame - writePos_;
        const int64_t size = drift < 0 ? -drift : drift;
        bool realign = lost || size > ForceFrames();
        if (!realign && size > DriftFrames()) {
            realign = !pcm || PeakOf(pcm, static_cast<size_t>(frames) * kChannels) < kQuietPeak;
        }
        if (realign) {
            jump = drift;
        } else {
            frame = writePos_;
        }
    }

    const int64_t slots = static_cast<int64_t>(chunks_.size());
    int64_t f = frame;
    uint32_t done = 0;
    while (done < frames) {
        if (f < 0) {  // audio from before the clock started
            uint32_t skip = static_cast<uint32_t>(std::min<int64_t>(-f, frames - done));
            f += skip;
            done += skip;
            continue;
        }
        int64_t index = f / chunkFrames_;
        int64_t offset = f % chunkFrames_;
        uint32_t n = static_cast<uint32_t>(std::min<int64_t>(frames - done, chunkFrames_ - offset));
        const float* src = pcm ? pcm + static_cast<size_t>(done) * kChannels : nullptr;
        bool loud = src && !IsSilent(src, static_cast<size_t>(n) * kChannels);

        Chunk& c = chunks_[static_cast<size_t>(index % slots)];
        if (loud) {
            if (c.index != index) {
                if (!c.data) c.data = static_cast<float*>(malloc(chunkBytes_));
                if (c.data) memset(c.data, 0, chunkBytes_);
                c.index = c.data ? index : -1;
            }
            if (c.data) memcpy(c.data + offset * kChannels, src, static_cast<size_t>(n) * kBytesPerFrame);
            lastSound_ = f + n;
        } else if (c.index == index && c.data) {
            // Only matters when rewriting a span that already held sound.
            memset(c.data + offset * kChannels, 0, static_cast<size_t>(n) * kBytesPerFrame);
        }

        if (index != lastChunk_) {
            lastChunk_ = index;
            FreeStale(index);
        }
        f += n;
        done += n;
    }
    writePos_ = frame + frames;

    ReleaseSRWLockExclusive(&lock_);
    return jump;
}

void RingBuffer::FreeStale(int64_t currentChunk) {
    const int64_t oldest = currentChunk - static_cast<int64_t>(chunks_.size()) + 1;
    for (Chunk& c : chunks_) {
        if (c.data && c.index < oldest) {
            free(c.data);
            c.data = nullptr;
            c.index = -1;
        }
    }
}

void RingBuffer::Read(int64_t start, int64_t frames, float* out) const {
    AcquireSRWLockShared(&lock_);

    const int64_t slots = static_cast<int64_t>(chunks_.size());
    int64_t f = start;
    int64_t done = 0;
    while (done < frames) {
        float* dst = out + done * kChannels;
        if (f < 0) {
            int64_t n = std::min(-f, frames - done);
            memset(dst, 0, static_cast<size_t>(n) * kBytesPerFrame);
            f += n;
            done += n;
            continue;
        }
        int64_t index = f / chunkFrames_;
        int64_t offset = f % chunkFrames_;
        int64_t n = std::min(frames - done, chunkFrames_ - offset);
        const Chunk& c = chunks_[static_cast<size_t>(index % slots)];
        if (c.index == index && c.data) {
            memcpy(dst, c.data + offset * kChannels, static_cast<size_t>(n) * kBytesPerFrame);
        } else {
            memset(dst, 0, static_cast<size_t>(n) * kBytesPerFrame);
        }
        f += n;
        done += n;
    }

    ReleaseSRWLockShared(&lock_);
}

int64_t RingBuffer::LastSoundFrame() const {
    AcquireSRWLockShared(&lock_);
    int64_t v = lastSound_;
    ReleaseSRWLockShared(&lock_);
    return v;
}

size_t RingBuffer::BytesInUse() const {
    AcquireSRWLockShared(&lock_);
    size_t bytes = 0;
    for (const Chunk& c : chunks_) {
        if (c.data) bytes += chunkBytes_;
    }
    ReleaseSRWLockShared(&lock_);
    return bytes;
}
