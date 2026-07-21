#pragma once

#include "skui_media.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace skui::ffmpeg::detail {

struct StreamMetadata {
    double durationSeconds = 0.0;
    double frameDurationSeconds = 1.0 / 30.0;
    int videoWidth = 0;
    int videoHeight = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool hasAlpha = false;
    std::string videoDecoderName;
};

struct DecodedVideoFrame {
    double presentationSeconds = 0.0;
    double durationSeconds = 0.0;
    bool hasAlpha = false;
    sk_sp<SkImage> image;
};

struct DecodedAudioChunk {
    double presentationSeconds = 0.0;
    int frameCount = 0;
    std::vector<float> samples;
};

struct DecodeBatch {
    std::vector<DecodedVideoFrame> videoFrames;
    std::vector<DecodedAudioChunk> audioChunks;
};

enum class DecodeStatus {
    Produced,
    EndOfStream,
    Interrupted,
    Failed,
};

class DecoderSession {
public:
    explicit DecoderSession(std::atomic_bool& cancelRequested);
    ~DecoderSession();

    DecoderSession(const DecoderSession&) = delete;
    DecoderSession& operator=(const DecoderSession&) = delete;

    bool open(const std::string& source, std::string& error);
    bool configureAudio(const AudioOutputFormat& format, std::string& error);
    bool seek(double seconds, std::string& error);
    DecodeStatus decodeNext(DecodeBatch& batch, std::string& error);

    [[nodiscard]] const StreamMetadata& metadata() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace skui::ffmpeg::detail
