#include "wav.h"

#include <algorithm>
#include <cstring>

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t v) { out.insert(out.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }
void Put16(std::vector<uint8_t>& out, uint16_t v) { out.insert(out.end(), {uint8_t(v), uint8_t(v >> 8)}); }
void PutTag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

// RIFF header up to and including the "data" chunk size.
std::vector<uint8_t> Header(bool isFloat, size_t frames) {
    const uint16_t bits = isFloat ? 32 : 16;
    const uint16_t blockAlign = kChannels * bits / 8;
    const uint32_t dataBytes = static_cast<uint32_t>(frames * blockAlign);
    const uint32_t fmtBytes = isFloat ? 18 : 16;
    const uint32_t factBytes = isFloat ? 12 : 0;  // non-PCM formats carry a "fact" chunk

    std::vector<uint8_t> h;
    PutTag(h, "RIFF");
    Put32(h, 4 + (8 + fmtBytes) + factBytes + (8 + dataBytes));
    PutTag(h, "WAVE");
    PutTag(h, "fmt ");
    Put32(h, fmtBytes);
    Put16(h, isFloat ? 3 : 1);  // WAVE_FORMAT_IEEE_FLOAT or PCM
    Put16(h, kChannels);
    Put32(h, SampleRate());
    Put32(h, SampleRate() * blockAlign);
    Put16(h, blockAlign);
    Put16(h, bits);
    if (isFloat) {
        Put16(h, 0);  // cbSize
        PutTag(h, "fact");
        Put32(h, 4);
        Put32(h, static_cast<uint32_t>(frames));
    }
    PutTag(h, "data");
    Put32(h, dataBytes);
    return h;
}

}  // namespace

bool WriteWav(const std::wstring& path, const float* pcm, size_t frames) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log(L"Could not create %s (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    std::vector<uint8_t> header = Header(true, frames);
    const DWORD dataBytes = static_cast<DWORD>(frames * kBytesPerFrame);
    DWORD written = 0;
    bool ok = WriteFile(file, header.data(), static_cast<DWORD>(header.size()), &written, nullptr) &&
              WriteFile(file, pcm, dataBytes, &written, nullptr) && written == dataBytes;
    CloseHandle(file);
    if (!ok) {
        Log(L"Write failed for %s", path.c_str());
        DeleteFileW(path.c_str());
    }
    return ok;
}

std::vector<uint8_t> MakePreviewWav(const float* pcm, size_t frames) {
    std::vector<uint8_t> wav = Header(false, frames);
    const size_t samples = frames * kChannels;
    const size_t base = wav.size();
    wav.resize(base + samples * 2);
    for (size_t i = 0; i < samples; ++i) {
        int v = static_cast<int>(std::clamp(pcm[i], -1.0f, 1.0f) * 32767.0f);
        int16_t s = static_cast<int16_t>(v);
        memcpy(wav.data() + base + i * 2, &s, 2);
    }
    return wav;
}
