// WAV encoding for Ephemeront's internal format (32-bit float stereo).
#pragma once

#include "common.h"

// Saves a clip losslessly as a 32-bit float WAV.
bool WriteWav(const std::wstring& path, const float* pcm, size_t frames);

// Builds an in-memory 16-bit WAV, for playback through PlaySound.
std::vector<uint8_t> MakePreviewWav(const float* pcm, size_t frames);
