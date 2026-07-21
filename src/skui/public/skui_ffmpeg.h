#pragma once

#include "skui_media.h"

namespace skui::ffmpeg {

[[nodiscard]] MediaPlayerFactory makeMediaPlayerFactory(
    AudioOutputFactory audioOutputFactory = {});

}  // namespace skui::ffmpeg
