// Functions the main tray app provides to its windows.
#pragma once

#include "common.h"

// Writes [start, start + frames) of each clip to the clips folder as WAV.
// Returns the paths of the files written.
std::vector<std::wstring> SaveClips(const std::vector<const Clip*>& clips, int64_t start, int64_t frames,
                                   bool skipSilent);

// Shows "Clip saved" (if notifications are on); clicking it shows the files.
void AppNotifySaved(const std::vector<std::wstring>& files, double seconds);
void AppSettingsChanged();
