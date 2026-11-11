// Functions the main tray app provides to its windows.
#pragma once

#include "common.h"

// Writes [start, start + frames) of each clip to the clips folder as WAV.
// Returns how many files were written.
int SaveClips(const std::vector<const Clip*>& clips, int64_t start, int64_t frames, bool skipSilent);

void AppNotifySaved(int files, double seconds);
void AppSettingsChanged();
