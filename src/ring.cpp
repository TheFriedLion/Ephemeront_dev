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

RingBuffer::RingBuffer()
    : chunkFrames_(SampleRate()), chunkBytes_(static_cast<size_t>(SampleRate()) * kBytesPerFrame) {}

RingBuffer::~RingBuffer() {
    for (auto& [index, data] : chunks_) free(data);
}

RingBuffer::WriteResult RingBuffer::Write(int64_t frame, const float* pcm, uint32_t frames, bool lost) {
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

    int64_t f = frame;
    uint32_t done = 0;
    while (done < frames) {
        if (f < 0) {  // audio from before the clock started
            uint32_t skip = static_cast<uint32_t>(std::min<int64_t>(-f, frames - done));
            f += skip;
            done += skip;
            continue;
        }
        const int64_t index = f / chunkFrames_;
        const int64_t offset = f % chunkFrames_;
        const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(frames - done, chunkFrames_ - offset));
        const float* src = pcm ? pcm + static_cast<size_t>(done) * kChannels : nullptr;
        const bool loud = src && !IsSilent(src, static_cast<size_t>(n) * kChannels);

        auto it = chunks_.find(index);
        if (loud) {
            if (it == chunks_.end()) {
                float* data = static_cast<float*>(calloc(1, chunkBytes_));
                if (data) it = chunks_.emplace(index, data).first;
            }
            if (it != chunks_.end()) {
                memcpy(it->second + offset * kChannels, src, static_cast<size_t>(n) * kBytesPerFrame);
            }
            lastSound_ = f + n;
        } else if (it != chunks_.end()) {
            // Only matters when rewriting a span that already held sound.
            memset(it->second + offset * kChannels, 0, static_cast<size_t>(n) * kBytesPerFrame);
        }
        f += n;
        done += n;
    }
    writePos_ = frame + frames;

    ReleaseSRWLockExclusive(&lock_);
    return {frame, jump};
}

void RingBuffer::Read(int64_t start, int64_t frames, float* out) const {
    AcquireSRWLockShared(&lock_);

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
        const int64_t index = f / chunkFrames_;
        const int64_t offset = f % chunkFrames_;
        const int64_t n = std::min(frames - done, chunkFrames_ - offset);
        auto it = chunks_.find(index);
        if (it != chunks_.end()) {
            memcpy(dst, it->second + offset * kChannels, static_cast<size_t>(n) * kBytesPerFrame);
        } else {
            memset(dst, 0, static_cast<size_t>(n) * kBytesPerFrame);
        }
        f += n;
        done += n;
    }

    ReleaseSRWLockShared(&lock_);
}

void RingBuffer::Prune(const std::vector<std::pair<int64_t, int64_t>>& keep, int64_t before) {
    AcquireSRWLockExclusive(&lock_);
    size_t k = 0;
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        const int64_t start = it->first * chunkFrames_, end = start + chunkFrames_;
        if (end > before) break;  // chunks are in order, so the rest are newer still
        while (k < keep.size() && keep[k].second <= start) ++k;
        const bool kept = k < keep.size() && keep[k].first < end;
        if (kept) {
            ++it;
        } else {
            free(it->second);
            it = chunks_.erase(it);
        }
    }
    ReleaseSRWLockExclusive(&lock_);
}

int64_t RingBuffer::LastSoundFrame() const {
    AcquireSRWLockShared(&lock_);
    int64_t v = lastSound_;
    ReleaseSRWLockShared(&lock_);
    return v;
}

size_t RingBuffer::BytesInUse() const {
    AcquireSRWLockShared(&lock_);
    size_t bytes = chunks_.size() * chunkBytes_;
    ReleaseSRWLockShared(&lock_);
    return bytes;
}
