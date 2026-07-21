#pragma once

#include "skui_media.h"

namespace skui::win32 {

[[nodiscard]] AudioOutputFactory makeWasapiAudioOutputFactory();

}  // namespace skui::win32
