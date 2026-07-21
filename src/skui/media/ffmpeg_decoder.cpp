#include "ffmpeg_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

namespace skui::ffmpeg::detail {
namespace {

constexpr double kDefaultFrameDurationSeconds = 1.0 / 30.0;

std::string ffmpegError(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context) {
            avformat_close_input(&context);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const {
        if (context) {
            avcodec_free_context(&context);
        }
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        if (packet) {
            av_packet_free(&packet);
        }
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const {
        sws_freeContext(context);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* context) const {
        if (context) {
            swr_free(&context);
        }
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

const AVCodec* findVideoDecoder(AVCodecID codecId, std::string& error) {
    const char* requiredName = nullptr;
    if (codecId == AV_CODEC_ID_VP8) {
        requiredName = "libvpx";
    } else if (codecId == AV_CODEC_ID_VP9) {
        requiredName = "libvpx-vp9";
    }

    if (requiredName) {
        const AVCodec* codec = avcodec_find_decoder_by_name(requiredName);
        if (!codec) {
            error = std::string("required transparent WebM decoder is unavailable: ") +
                    requiredName;
        }
        return codec;
    }
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        error = "no software video decoder is available for this codec";
    }
    return codec;
}

double rationalSeconds(int64_t value, AVRational timeBase) {
    if (value == AV_NOPTS_VALUE) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(value) * av_q2d(timeBase);
}

void premultiplyBgra(std::vector<uint8_t>& pixels) {
    for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
        const uint32_t alpha = pixels[index + 3];
        pixels[index] = static_cast<uint8_t>((pixels[index] * alpha + 127) / 255);
        pixels[index + 1] =
            static_cast<uint8_t>((pixels[index + 1] * alpha + 127) / 255);
        pixels[index + 2] =
            static_cast<uint8_t>((pixels[index + 2] * alpha + 127) / 255);
    }
}

}  // namespace

class DecoderSession::Impl {
public:
    explicit Impl(std::atomic_bool& cancelRequested)
        : cancelRequested_(cancelRequested) {}

    bool open(const std::string& source, std::string& error) {
        AVFormatContext* rawFormat = avformat_alloc_context();
        if (!rawFormat) {
            error = "avformat_alloc_context failed";
            return false;
        }
        rawFormat->interrupt_callback.callback = &Impl::interruptCallback;
        rawFormat->interrupt_callback.opaque = this;

        const int openResult = avformat_open_input(&rawFormat, source.c_str(), nullptr, nullptr);
        if (openResult < 0) {
            FormatContextDeleter{}(rawFormat);
            error = "avformat_open_input failed: " + ffmpegError(openResult);
            return false;
        }
        format_.reset(rawFormat);

        const int streamInfoResult = avformat_find_stream_info(format_.get(), nullptr);
        if (streamInfoResult < 0) {
            error = "avformat_find_stream_info failed: " + ffmpegError(streamInfoResult);
            return false;
        }

        if (!openVideo(error) || !openAudio(error)) {
            return false;
        }
        if (!metadata_.hasVideo) {
            error = "media source does not contain a playable video stream";
            return false;
        }

        if (format_->duration != AV_NOPTS_VALUE && format_->duration > 0) {
            metadata_.durationSeconds =
                static_cast<double>(format_->duration) / AV_TIME_BASE;
        } else {
            const AVStream* videoStream = format_->streams[videoStreamIndex_];
            metadata_.durationSeconds =
                std::max(0.0, rationalSeconds(videoStream->duration, videoStream->time_base));
        }
        timelineOriginSeconds_ =
            format_->start_time == AV_NOPTS_VALUE
                ? streamStartSeconds(format_->streams[videoStreamIndex_])
                : static_cast<double>(format_->start_time) / AV_TIME_BASE;
        if (!std::isfinite(timelineOriginSeconds_)) {
            timelineOriginSeconds_ = 0.0;
        }

        packet_.reset(av_packet_alloc());
        decodeFrame_.reset(av_frame_alloc());
        if (!packet_ || !decodeFrame_) {
            error = "unable to allocate FFmpeg packet or frame";
            return false;
        }
        return true;
    }

    bool configureAudio(const AudioOutputFormat& format, std::string& error) {
        audioOutputFormat_ = format;
        if (!audioCodec_) {
            return true;
        }
        if (format.sampleRate <= 0 || format.channelCount <= 0) {
            error = "audio output returned an invalid format";
            return false;
        }

        AVChannelLayout outputLayout{};
        av_channel_layout_default(&outputLayout, format.channelCount);
        SwrContext* rawResampler = nullptr;
        const int allocateResult = swr_alloc_set_opts2(
            &rawResampler,
            &outputLayout,
            AV_SAMPLE_FMT_FLT,
            format.sampleRate,
            &audioCodec_->ch_layout,
            audioCodec_->sample_fmt,
            audioCodec_->sample_rate,
            0,
            nullptr);
        av_channel_layout_uninit(&outputLayout);
        if (allocateResult < 0 || !rawResampler) {
            error = "swr_alloc_set_opts2 failed: " + ffmpegError(allocateResult);
            return false;
        }
        audioResampler_.reset(rawResampler);
        const int initializeResult = swr_init(audioResampler_.get());
        if (initializeResult < 0) {
            error = "swr_init failed: " + ffmpegError(initializeResult);
            return false;
        }
        return true;
    }

    bool seek(double seconds, std::string& error) {
        if (!format_) {
            error = "decoder is not open";
            return false;
        }
        const double clampedSeconds = std::max(0.0, seconds);
        const int64_t target = static_cast<int64_t>(std::llround(
            (timelineOriginSeconds_ + clampedSeconds) * AV_TIME_BASE));
        const int seekResult = avformat_seek_file(
            format_.get(), -1, std::numeric_limits<int64_t>::min(), target,
            std::numeric_limits<int64_t>::max(), AVSEEK_FLAG_BACKWARD);
        if (seekResult < 0) {
            error = "avformat_seek_file failed: " + ffmpegError(seekResult);
            return false;
        }
        avcodec_flush_buffers(videoCodec_.get());
        if (audioCodec_) {
            avcodec_flush_buffers(audioCodec_.get());
        }
        if (audioResampler_) {
            swr_close(audioResampler_.get());
            const int initializeResult = swr_init(audioResampler_.get());
            if (initializeResult < 0) {
                error = "swr_init after seek failed: " + ffmpegError(initializeResult);
                return false;
            }
        }
        demuxEnded_ = false;
        videoFlushSent_ = false;
        audioFlushSent_ = false;
        resamplerFlushed_ = false;
        nextVideoSeconds_ = clampedSeconds;
        nextAudioSeconds_ = clampedSeconds;
        av_packet_unref(packet_.get());
        av_frame_unref(decodeFrame_.get());
        return true;
    }

    DecodeStatus decodeNext(DecodeBatch& batch, std::string& error) {
        batch = {};
        if (cancelRequested_.load(std::memory_order_relaxed)) {
            return DecodeStatus::Interrupted;
        }

        if (!demuxEnded_) {
            const int readResult = av_read_frame(format_.get(), packet_.get());
            if (readResult >= 0) {
                const DecodeStatus status = decodePacket(batch, error);
                av_packet_unref(packet_.get());
                return status;
            }
            if (readResult != AVERROR_EOF) {
                if (cancelRequested_.load(std::memory_order_relaxed) ||
                    readResult == AVERROR_EXIT) {
                    return DecodeStatus::Interrupted;
                }
                error = "av_read_frame failed: " + ffmpegError(readResult);
                return DecodeStatus::Failed;
            }
            demuxEnded_ = true;
        }

        const DecodeStatus flushStatus = flushDecoders(batch, error);
        if (flushStatus == DecodeStatus::Produced ||
            flushStatus == DecodeStatus::Failed) {
            return flushStatus;
        }
        return DecodeStatus::EndOfStream;
    }

    [[nodiscard]] const StreamMetadata& metadata() const {
        return metadata_;
    }

private:
    static int interruptCallback(void* opaque) {
        const auto* self = static_cast<const Impl*>(opaque);
        return self->cancelRequested_.load(std::memory_order_relaxed) ? 1 : 0;
    }

    static double streamStartSeconds(const AVStream* stream) {
        if (stream->start_time == AV_NOPTS_VALUE) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return rationalSeconds(stream->start_time, stream->time_base);
    }

    bool openVideo(std::string& error) {
        videoStreamIndex_ = av_find_best_stream(
            format_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStreamIndex_ < 0) {
            return true;
        }

        AVStream* stream = format_->streams[videoStreamIndex_];
        const AVCodec* decoder = findVideoDecoder(stream->codecpar->codec_id, error);
        if (!decoder) {
            return false;
        }
        CodecContextPtr codec(avcodec_alloc_context3(decoder));
        if (!codec) {
            error = "avcodec_alloc_context3 failed for video";
            return false;
        }
        const int parametersResult =
            avcodec_parameters_to_context(codec.get(), stream->codecpar);
        if (parametersResult < 0) {
            error = "avcodec_parameters_to_context failed for video: " +
                    ffmpegError(parametersResult);
            return false;
        }
        codec->thread_count = 0;
        const int openResult = avcodec_open2(codec.get(), decoder, nullptr);
        if (openResult < 0) {
            error = "avcodec_open2 failed for video decoder " +
                    std::string(decoder->name) + ": " + ffmpegError(openResult);
            return false;
        }

        const AVRational guessedRate = av_guess_frame_rate(format_.get(), stream, nullptr);
        const double frameRate = av_q2d(guessedRate);
        metadata_.frameDurationSeconds =
            frameRate > 0.0 ? 1.0 / frameRate : kDefaultFrameDurationSeconds;
        metadata_.videoWidth = codec->width;
        metadata_.videoHeight = codec->height;
        metadata_.hasVideo = true;
        const AVDictionaryEntry* alphaMode =
            av_dict_get(stream->metadata, "alpha_mode", nullptr, 0);
        expectsAlpha_ = alphaMode && std::string_view(alphaMode->value) != "0";
        metadata_.hasAlpha = expectsAlpha_;
        metadata_.videoDecoderName = decoder->name;
        videoCodec_ = std::move(codec);
        return true;
    }

    bool openAudio(std::string& error) {
        audioStreamIndex_ = av_find_best_stream(
            format_.get(), AVMEDIA_TYPE_AUDIO, -1, videoStreamIndex_, nullptr, 0);
        if (audioStreamIndex_ < 0) {
            return true;
        }

        AVStream* stream = format_->streams[audioStreamIndex_];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            error = "no software audio decoder is available for this codec";
            return false;
        }
        CodecContextPtr codec(avcodec_alloc_context3(decoder));
        if (!codec) {
            error = "avcodec_alloc_context3 failed for audio";
            return false;
        }
        const int parametersResult =
            avcodec_parameters_to_context(codec.get(), stream->codecpar);
        if (parametersResult < 0) {
            error = "avcodec_parameters_to_context failed for audio: " +
                    ffmpegError(parametersResult);
            return false;
        }
        const int openResult = avcodec_open2(codec.get(), decoder, nullptr);
        if (openResult < 0) {
            error = "avcodec_open2 failed for audio: " + ffmpegError(openResult);
            return false;
        }
        metadata_.hasAudio = true;
        audioCodec_ = std::move(codec);
        return true;
    }

    DecodeStatus decodePacket(DecodeBatch& batch, std::string& error) {
        if (packet_->stream_index == videoStreamIndex_) {
            const int sendResult = avcodec_send_packet(videoCodec_.get(), packet_.get());
            if (sendResult < 0) {
                error = "avcodec_send_packet failed for video: " +
                        ffmpegError(sendResult);
                return DecodeStatus::Failed;
            }
            return drainVideo(batch, error);
        }
        if (audioCodec_ && packet_->stream_index == audioStreamIndex_) {
            const int sendResult = avcodec_send_packet(audioCodec_.get(), packet_.get());
            if (sendResult < 0) {
                error = "avcodec_send_packet failed for audio: " +
                        ffmpegError(sendResult);
                return DecodeStatus::Failed;
            }
            return drainAudio(batch, error);
        }
        return DecodeStatus::Produced;
    }

    DecodeStatus drainVideo(DecodeBatch& batch, std::string& error) {
        while (true) {
            const int receiveResult =
                avcodec_receive_frame(videoCodec_.get(), decodeFrame_.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return DecodeStatus::Produced;
            }
            if (receiveResult < 0) {
                error = "avcodec_receive_frame failed for video: " +
                        ffmpegError(receiveResult);
                return DecodeStatus::Failed;
            }

            DecodedVideoFrame frame;
            if (!convertVideoFrame(frame, error)) {
                av_frame_unref(decodeFrame_.get());
                return DecodeStatus::Failed;
            }
            batch.videoFrames.push_back(std::move(frame));
            av_frame_unref(decodeFrame_.get());
        }
    }

    DecodeStatus drainAudio(DecodeBatch& batch, std::string& error) {
        while (true) {
            const int receiveResult =
                avcodec_receive_frame(audioCodec_.get(), decodeFrame_.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return DecodeStatus::Produced;
            }
            if (receiveResult < 0) {
                error = "avcodec_receive_frame failed for audio: " +
                        ffmpegError(receiveResult);
                return DecodeStatus::Failed;
            }

            if (audioResampler_) {
                DecodedAudioChunk chunk;
                if (!convertAudioFrame(chunk, error)) {
                    av_frame_unref(decodeFrame_.get());
                    return DecodeStatus::Failed;
                }
                if (chunk.frameCount > 0) {
                    batch.audioChunks.push_back(std::move(chunk));
                }
            }
            av_frame_unref(decodeFrame_.get());
        }
    }

    DecodeStatus flushDecoders(DecodeBatch& batch, std::string& error) {
        if (!videoFlushSent_) {
            const int sendResult = avcodec_send_packet(videoCodec_.get(), nullptr);
            if (sendResult < 0 && sendResult != AVERROR_EOF) {
                error = "unable to flush video decoder: " + ffmpegError(sendResult);
                return DecodeStatus::Failed;
            }
            videoFlushSent_ = true;
        }
        const DecodeStatus videoStatus = drainVideo(batch, error);
        if (videoStatus == DecodeStatus::Failed) {
            return videoStatus;
        }

        if (audioCodec_ && !audioFlushSent_) {
            const int sendResult = avcodec_send_packet(audioCodec_.get(), nullptr);
            if (sendResult < 0 && sendResult != AVERROR_EOF) {
                error = "unable to flush audio decoder: " + ffmpegError(sendResult);
                return DecodeStatus::Failed;
            }
            audioFlushSent_ = true;
        }
        if (audioCodec_) {
            const DecodeStatus audioStatus = drainAudio(batch, error);
            if (audioStatus == DecodeStatus::Failed) {
                return audioStatus;
            }
        }
        if (!flushAudioResampler(batch, error)) {
            return DecodeStatus::Failed;
        }
        return batch.videoFrames.empty() && batch.audioChunks.empty()
                   ? DecodeStatus::EndOfStream
                   : DecodeStatus::Produced;
    }

    bool convertVideoFrame(DecodedVideoFrame& output, std::string& error) {
        const AVPixelFormat pixelFormat =
            static_cast<AVPixelFormat>(decodeFrame_->format);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixelFormat);
        const bool decodedAlpha = descriptor &&
                                  (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
        if (expectsAlpha_ && !decodedAlpha) {
            error = "media declares an alpha channel, but the decoder returned "
                    "an opaque pixel format";
            return false;
        }
        metadata_.hasAlpha = metadata_.hasAlpha || decodedAlpha;
        output.hasAlpha = decodedAlpha;

        SwsContext* rawScaler = sws_getCachedContext(
            videoScaler_.release(),
            decodeFrame_->width,
            decodeFrame_->height,
            pixelFormat,
            decodeFrame_->width,
            decodeFrame_->height,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        videoScaler_.reset(rawScaler);
        if (!videoScaler_) {
            error = "sws_getCachedContext failed";
            return false;
        }

        const size_t rowBytes = static_cast<size_t>(decodeFrame_->width) * 4;
        std::vector<uint8_t> pixels(
            rowBytes * static_cast<size_t>(decodeFrame_->height));
        uint8_t* destination[] = {pixels.data()};
        const int destinationLines[] = {static_cast<int>(rowBytes)};
        const int scaledHeight = sws_scale(
            videoScaler_.get(),
            decodeFrame_->data,
            decodeFrame_->linesize,
            0,
            decodeFrame_->height,
            destination,
            destinationLines);
        if (scaledHeight != decodeFrame_->height) {
            error = "sws_scale did not produce a complete video frame";
            return false;
        }
        premultiplyBgra(pixels);

        const SkImageInfo imageInfo = SkImageInfo::Make(
            decodeFrame_->width,
            decodeFrame_->height,
            kBGRA_8888_SkColorType,
            kPremul_SkAlphaType);
        const SkPixmap pixmap(imageInfo, pixels.data(), rowBytes);
        output.image = SkImages::RasterFromPixmapCopy(pixmap);
        if (!output.image) {
            error = "Skia could not create a raster image for a decoded frame";
            return false;
        }

        const AVStream* stream = format_->streams[videoStreamIndex_];
        double presentationSeconds = rationalSeconds(
            decodeFrame_->best_effort_timestamp, stream->time_base);
        if (std::isfinite(presentationSeconds)) {
            presentationSeconds = std::max(0.0, presentationSeconds - timelineOriginSeconds_);
        } else {
            presentationSeconds = nextVideoSeconds_;
        }
        double durationSeconds = rationalSeconds(decodeFrame_->duration, stream->time_base);
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
            durationSeconds = metadata_.frameDurationSeconds;
        }
        output.presentationSeconds = presentationSeconds;
        output.durationSeconds = durationSeconds;
        nextVideoSeconds_ = presentationSeconds + durationSeconds;
        return true;
    }

    bool convertAudioFrame(DecodedAudioChunk& output, std::string& error) {
        const int outputCapacity =
            swr_get_out_samples(audioResampler_.get(), decodeFrame_->nb_samples);
        if (outputCapacity < 0) {
            error = "swr_get_out_samples failed: " + ffmpegError(outputCapacity);
            return false;
        }
        output.samples.resize(
            static_cast<size_t>(outputCapacity) * audioOutputFormat_.channelCount);
        uint8_t* destination =
            reinterpret_cast<uint8_t*>(output.samples.data());
        const int convertedFrames = swr_convert(
            audioResampler_.get(),
            &destination,
            outputCapacity,
            reinterpret_cast<const uint8_t* const*>(decodeFrame_->extended_data),
            decodeFrame_->nb_samples);
        if (convertedFrames < 0) {
            error = "swr_convert failed: " + ffmpegError(convertedFrames);
            return false;
        }
        output.frameCount = convertedFrames;
        output.samples.resize(
            static_cast<size_t>(convertedFrames) * audioOutputFormat_.channelCount);

        const AVStream* stream = format_->streams[audioStreamIndex_];
        double presentationSeconds = rationalSeconds(
            decodeFrame_->best_effort_timestamp, stream->time_base);
        if (std::isfinite(presentationSeconds)) {
            presentationSeconds = std::max(0.0, presentationSeconds - timelineOriginSeconds_);
        } else {
            presentationSeconds = nextAudioSeconds_;
        }
        output.presentationSeconds = presentationSeconds;
        nextAudioSeconds_ =
            presentationSeconds + static_cast<double>(convertedFrames) /
                                      audioOutputFormat_.sampleRate;
        return true;
    }

    bool flushAudioResampler(DecodeBatch& batch, std::string& error) {
        if (!audioResampler_ || resamplerFlushed_) {
            return true;
        }
        const int outputCapacity = swr_get_out_samples(audioResampler_.get(), 0);
        if (outputCapacity < 0) {
            error = "swr_get_out_samples failed while flushing: " +
                    ffmpegError(outputCapacity);
            return false;
        }
        if (outputCapacity == 0) {
            resamplerFlushed_ = true;
            return true;
        }

        DecodedAudioChunk chunk;
        chunk.presentationSeconds = nextAudioSeconds_;
        chunk.samples.resize(
            static_cast<size_t>(outputCapacity) * audioOutputFormat_.channelCount);
        uint8_t* destination = reinterpret_cast<uint8_t*>(chunk.samples.data());
        const int convertedFrames = swr_convert(
            audioResampler_.get(), &destination, outputCapacity, nullptr, 0);
        if (convertedFrames < 0) {
            error = "swr_convert failed while flushing: " +
                    ffmpegError(convertedFrames);
            return false;
        }
        chunk.frameCount = convertedFrames;
        chunk.samples.resize(
            static_cast<size_t>(convertedFrames) * audioOutputFormat_.channelCount);
        nextAudioSeconds_ +=
            static_cast<double>(convertedFrames) / audioOutputFormat_.sampleRate;
        if (convertedFrames > 0) {
            batch.audioChunks.push_back(std::move(chunk));
        } else {
            resamplerFlushed_ = true;
        }
        return true;
    }

    std::atomic_bool& cancelRequested_;
    StreamMetadata metadata_;
    FormatContextPtr format_;
    CodecContextPtr videoCodec_;
    CodecContextPtr audioCodec_;
    PacketPtr packet_;
    FramePtr decodeFrame_;
    SwsContextPtr videoScaler_;
    SwrContextPtr audioResampler_;
    AudioOutputFormat audioOutputFormat_;
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;
    double timelineOriginSeconds_ = 0.0;
    double nextVideoSeconds_ = 0.0;
    double nextAudioSeconds_ = 0.0;
    bool demuxEnded_ = false;
    bool videoFlushSent_ = false;
    bool audioFlushSent_ = false;
    bool resamplerFlushed_ = false;
    bool expectsAlpha_ = false;
};

DecoderSession::DecoderSession(std::atomic_bool& cancelRequested)
    : impl_(std::make_unique<Impl>(cancelRequested)) {}

DecoderSession::~DecoderSession() = default;

bool DecoderSession::open(const std::string& source, std::string& error) {
    return impl_->open(source, error);
}

bool DecoderSession::configureAudio(const AudioOutputFormat& format,
                                    std::string& error) {
    return impl_->configureAudio(format, error);
}

bool DecoderSession::seek(double seconds, std::string& error) {
    return impl_->seek(seconds, error);
}

DecodeStatus DecoderSession::decodeNext(DecodeBatch& batch, std::string& error) {
    return impl_->decodeNext(batch, error);
}

const StreamMetadata& DecoderSession::metadata() const {
    return impl_->metadata();
}

}  // namespace skui::ffmpeg::detail
