// Clip Picker: a small window for choosing which part of the buffer to keep,
// and from which sources.
#pragma once

#include "common.h"

// Opens the picker over the whole buffer, live. Returns false if nothing has made
// sound within the buffer yet.
bool PickerOpen(HINSTANCE instance, int64_t defaultFrames);
bool PickerIsOpen();
void PickerFocus();
void PickerClose();
