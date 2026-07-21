#include "skui_win32_audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace skui::win32 {
namespace {

using Microsoft::WRL::ComPtr;

class Win32Handle {
public:
    explicit Win32Handle(HANDLE handle = nullptr) : handle_(handle) {}

    ~Win32Handle() {
        if (handle_) {
            CloseHandle(handle_);
        }
    }

    Win32Handle(const Win32Handle&) = delete;
    Win32Handle& operator=(const Win32Handle&) = delete;

    [[nodiscard]] HANDLE get() const {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

std::string hresultError(std::string_view operation, HRESULT result) {
    return std::string(operation) + " failed with HRESULT " +
           std::to_string(static_cast<unsigned long>(result));
}

enum class AudioCommand {
    None,
    Start,
    Pause,
    Flush,
    Stop,
};

struct MediaFrameInterval {
    uint64_t startFrame = 0;
    uint64_t endFrame = 0;
};

constexpr size_t kMaximumMediaIntervals = 64;

class WasapiAudioOutput final : public AudioOutput {
public:
    WasapiAudioOutput()
        : controlEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    ~WasapiAudioOutput() override {
        stop();
    }

    bool configure(const AudioOutputFormat& format,
                   double bufferSeconds) override {
        stop();
        if (!controlEvent_.get()) {
            setError("CreateEventW failed for WASAPI control event");
            return false;
        }
        if (format.sampleRate <= 0 || format.channelCount <= 0 ||
            !std::isfinite(bufferSeconds) || bufferSeconds <= 0.0) {
            setError("invalid WASAPI output format or buffer duration");
            return false;
        }

        ringResetting_.store(true, std::memory_order_release);
        while (activeWriters_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        const size_t capacityFrames = std::max<size_t>(
            1, static_cast<size_t>(std::ceil(bufferSeconds * format.sampleRate)));
        samples_.assign(capacityFrames * format.channelCount, 0.0f);
        capacityFrames_.store(capacityFrames, std::memory_order_relaxed);
        formatSampleRate_.store(format.sampleRate, std::memory_order_relaxed);
        formatChannelCount_.store(format.channelCount, std::memory_order_relaxed);
        readCursor_.store(0, std::memory_order_relaxed);
        writeCursor_.store(0, std::memory_order_relaxed);
        ringResetting_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(controlMutex_);
            initializationFinished_ = false;
            initializationSucceeded_ = false;
            threadExited_ = false;
            pendingCommand_ = AudioCommand::None;
            commandSerial_ = 0;
            processedSerial_ = 0;
            lastCommandSucceeded_ = true;
        }
        submittedFrames_.store(0, std::memory_order_relaxed);
        devicePaddingFrames_.store(0, std::memory_order_relaxed);
        deviceMediaFrames_.store(0, std::memory_order_relaxed);
        playedMediaFrames_.store(0, std::memory_order_relaxed);
        mediaIntervalOverflow_.store(false, std::memory_order_relaxed);
        muted_.store(false, std::memory_order_relaxed);
        worker_ = std::jthread([this] {
            audioThreadMain();
        });

        std::unique_lock lock(controlMutex_);
        initializationChanged_.wait(lock, [this] {
            return initializationFinished_;
        });
        return initializationSucceeded_;
    }

    size_t write(std::span<const float> interleavedSamples) override {
        if (ringResetting_.load(std::memory_order_acquire)) {
            return 0;
        }
        activeWriters_.fetch_add(1, std::memory_order_acquire);
        if (ringResetting_.load(std::memory_order_acquire)) {
            activeWriters_.fetch_sub(1, std::memory_order_release);
            return 0;
        }
        const size_t channelCount = static_cast<size_t>(
            formatChannelCount_.load(std::memory_order_relaxed));
        const size_t capacityFrames =
            capacityFrames_.load(std::memory_order_relaxed);
        if (channelCount == 0 || capacityFrames == 0) {
            activeWriters_.fetch_sub(1, std::memory_order_release);
            return 0;
        }
        const size_t inputFrames = interleavedSamples.size() / channelCount;
        const uint64_t writeCursor = writeCursor_.load(std::memory_order_relaxed);
        const uint64_t readCursor = readCursor_.load(std::memory_order_acquire);
        const size_t bufferedFrames = static_cast<size_t>(writeCursor - readCursor);
        const size_t framesToWrite =
            std::min(inputFrames, capacityFrames - bufferedFrames);
        for (size_t frame = 0; frame < framesToWrite; ++frame) {
            const size_t destinationFrame =
                static_cast<size_t>(writeCursor + frame) % capacityFrames;
            const size_t destinationOffset = destinationFrame * channelCount;
            const size_t sourceOffset = frame * channelCount;
            std::copy_n(interleavedSamples.data() + sourceOffset,
                        channelCount,
                        samples_.data() + destinationOffset);
        }
        writeCursor_.store(writeCursor + framesToWrite,
                           std::memory_order_release);
        activeWriters_.fetch_sub(1, std::memory_order_release);
        return framesToWrite;
    }

    bool start() override {
        return sendCommand(AudioCommand::Start);
    }

    void pause() override {
        (void)sendCommand(AudioCommand::Pause);
    }

    void flush() override {
        (void)sendCommand(AudioCommand::Flush);
    }

    void stop() override {
        if (!worker_.joinable()) {
            return;
        }
        (void)sendCommand(AudioCommand::Stop);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void setMuted(bool muted) override {
        muted_.store(muted, std::memory_order_relaxed);
    }

    [[nodiscard]] AudioOutputFormat format() const override {
        return AudioOutputFormat{
            formatSampleRate_.load(std::memory_order_relaxed),
            formatChannelCount_.load(std::memory_order_relaxed),
        };
    }

    [[nodiscard]] uint64_t playedFrames() const override {
        return playedMediaFrames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t bufferedFrames() const override {
        const uint64_t writeCursor = writeCursor_.load(std::memory_order_acquire);
        const uint64_t readCursor = readCursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(writeCursor - readCursor) +
               static_cast<size_t>(
                   deviceMediaFrames_.load(std::memory_order_relaxed));
    }

    [[nodiscard]] size_t capacityFrames() const override {
        return capacityFrames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string lastError() const override {
        if (mediaIntervalOverflow_.load(std::memory_order_relaxed)) {
            return "WASAPI media interval capacity was exceeded";
        }
        std::lock_guard lock(errorMutex_);
        return lastError_;
    }

private:
    bool sendCommand(AudioCommand command) {
        std::unique_lock lock(controlMutex_);
        if (!worker_.joinable() || threadExited_) {
            return command == AudioCommand::Stop;
        }
        pendingCommand_ = command;
        const uint64_t serial = ++commandSerial_;
        SetEvent(controlEvent_.get());
        commandChanged_.wait(lock, [this, serial] {
            return processedSerial_ >= serial || threadExited_;
        });
        return processedSerial_ >= serial && lastCommandSucceeded_;
    }

    void audioThreadMain() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            finishInitialization(false, hresultError("CoInitializeEx", comResult));
            finishThread();
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioRenderClient> renderClient;
        Win32Handle renderEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        UINT32 deviceBufferFrames = 0;
        std::string error;
        if (!initializeWasapi(enumerator,
                              device,
                              audioClient,
                              renderClient,
                              renderEvent.get(),
                              deviceBufferFrames,
                              error)) {
            finishInitialization(false, std::move(error));
            if (uninitializeCom) {
                CoUninitialize();
            }
            finishThread();
            return;
        }
        finishInitialization(true, {});

        DWORD mmcssTaskIndex = 0;
        HANDLE mmcssHandle =
            AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
        const HANDLE events[] = {controlEvent_.get(), renderEvent.get()};
        bool running = false;
        bool keepRunning = true;
        while (keepRunning) {
            const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) {
                keepRunning = processControlCommand(audioClient.Get(), running);
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                // Stop 之后可能残留一次渲染事件。暂停状态下忽略它，工作线程必须
                // 保持存活，以便后续重新缓冲完成后还能接收 Start。
                if (running &&
                    !renderAvailableFrames(audioClient.Get(),
                                           renderClient.Get(),
                                           deviceBufferFrames)) {
                    keepRunning = false;
                }
            } else {
                setError("unexpected WASAPI wait result " +
                         std::to_string(waitResult) + ", last error " +
                         std::to_string(GetLastError()));
                keepRunning = false;
            }
        }
        if (running) {
            audioClient->Stop();
        }
        if (mmcssHandle) {
            AvRevertMmThreadCharacteristics(mmcssHandle);
        }
        if (uninitializeCom) {
            CoUninitialize();
        }
        finishThread();
    }

    bool initializeWasapi(ComPtr<IMMDeviceEnumerator>& enumerator,
                          ComPtr<IMMDevice>& device,
                          ComPtr<IAudioClient>& audioClient,
                          ComPtr<IAudioRenderClient>& renderClient,
                          HANDLE renderEvent,
                          UINT32& deviceBufferFrames,
                          std::string& error) {
        if (!renderEvent) {
            error = "CreateEventW failed for WASAPI render event";
            return false;
        }
        HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(enumerator.ReleaseAndGetAddressOf()));
        if (FAILED(result)) {
            error = hresultError("CoCreateInstance(MMDeviceEnumerator)", result);
            return false;
        }
        result = enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, device.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            error = hresultError("GetDefaultAudioEndpoint", result);
            return false;
        }
        result = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(audioClient.ReleaseAndGetAddressOf()));
        if (FAILED(result)) {
            error = hresultError("IMMDevice::Activate(IAudioClient)", result);
            return false;
        }

        const AudioOutputFormat outputFormat = format();
        WAVEFORMATEX waveFormat{};
        waveFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        waveFormat.nChannels = static_cast<WORD>(outputFormat.channelCount);
        waveFormat.nSamplesPerSec = static_cast<DWORD>(outputFormat.sampleRate);
        waveFormat.wBitsPerSample = 32;
        waveFormat.nBlockAlign = static_cast<WORD>(
            waveFormat.nChannels * waveFormat.wBitsPerSample / 8);
        waveFormat.nAvgBytesPerSec =
            waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        waveFormat.cbSize = 0;
        constexpr DWORD kStreamFlags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_NOPERSIST |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        result = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            kStreamFlags,
            0,
            0,
            &waveFormat,
            nullptr);
        if (FAILED(result)) {
            error = hresultError("IAudioClient::Initialize", result);
            return false;
        }
        result = audioClient->GetBufferSize(&deviceBufferFrames);
        if (FAILED(result)) {
            error = hresultError("IAudioClient::GetBufferSize", result);
            return false;
        }
        result = audioClient->SetEventHandle(renderEvent);
        if (FAILED(result)) {
            error = hresultError("IAudioClient::SetEventHandle", result);
            return false;
        }
        result = audioClient->GetService(
            IID_PPV_ARGS(renderClient.ReleaseAndGetAddressOf()));
        if (FAILED(result)) {
            error = hresultError("IAudioClient::GetService", result);
            return false;
        }
        return true;
    }

    bool processControlCommand(IAudioClient* audioClient, bool& running) {
        AudioCommand command = AudioCommand::None;
        uint64_t serial = 0;
        {
            std::lock_guard lock(controlMutex_);
            command = pendingCommand_;
            pendingCommand_ = AudioCommand::None;
            serial = commandSerial_;
        }

        HRESULT result = S_OK;
        bool keepRunning = true;
        if (command == AudioCommand::Start && !running) {
            result = audioClient->Start();
            running = SUCCEEDED(result);
        } else if (command == AudioCommand::Pause && running) {
            result = audioClient->Stop();
            if (SUCCEEDED(result)) {
                running = false;
                updateDevicePadding(audioClient);
            }
        } else if (command == AudioCommand::Flush) {
            if (running) {
                result = audioClient->Stop();
                running = false;
            }
            if (SUCCEEDED(result)) {
                result = audioClient->Reset();
            }
            if (SUCCEEDED(result)) {
                clearRingBuffer();
                mediaIntervalStart_ = 0;
                mediaIntervalCount_ = 0;
                submittedFrames_.store(0, std::memory_order_relaxed);
                devicePaddingFrames_.store(0, std::memory_order_relaxed);
                deviceMediaFrames_.store(0, std::memory_order_relaxed);
                playedMediaFrames_.store(0, std::memory_order_relaxed);
            }
        } else if (command == AudioCommand::Stop) {
            if (running) {
                result = audioClient->Stop();
                running = false;
            }
            keepRunning = false;
        }

        if (FAILED(result)) {
            setError(hresultError("WASAPI control command", result));
        }
        {
            std::lock_guard lock(controlMutex_);
            lastCommandSucceeded_ = SUCCEEDED(result);
            processedSerial_ = serial;
        }
        commandChanged_.notify_all();
        return keepRunning;
    }

    bool renderAvailableFrames(IAudioClient* audioClient,
                               IAudioRenderClient* renderClient,
                               UINT32 deviceBufferFrames) {
        UINT32 paddingFrames = 0;
        HRESULT result = audioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(result)) {
            setError(hresultError("IAudioClient::GetCurrentPadding", result));
            return false;
        }
        devicePaddingFrames_.store(paddingFrames, std::memory_order_relaxed);
        const uint64_t submittedBefore =
            submittedFrames_.load(std::memory_order_relaxed);
        const uint64_t playedDeviceFrames =
            submittedBefore >= paddingFrames ? submittedBefore - paddingFrames : 0;
        updatePlayedMediaFrames(playedDeviceFrames);
        const UINT32 availableFrames = deviceBufferFrames - paddingFrames;
        if (availableFrames == 0) {
            return true;
        }

        BYTE* destination = nullptr;
        result = renderClient->GetBuffer(availableFrames, &destination);
        if (FAILED(result)) {
            setError(hresultError("IAudioRenderClient::GetBuffer", result));
            return false;
        }

        const bool muted = muted_.load(std::memory_order_relaxed);
        const size_t copiedFrames = copyFromRingBuffer(
            reinterpret_cast<float*>(destination), availableFrames, muted);
        const size_t channelCount = static_cast<size_t>(
            formatChannelCount_.load(std::memory_order_relaxed));
        if (copiedFrames < availableFrames) {
            std::fill_n(
                reinterpret_cast<float*>(destination) + copiedFrames * channelCount,
                (availableFrames - copiedFrames) * channelCount,
                0.0f);
        }
        result = renderClient->ReleaseBuffer(availableFrames, 0);
        if (FAILED(result)) {
            setError(hresultError("IAudioRenderClient::ReleaseBuffer", result));
            return false;
        }

        if (copiedFrames > 0) {
            const bool intervalStored = pushMediaInterval(MediaFrameInterval{
                submittedBefore,
                submittedBefore + copiedFrames,
            });
            if (intervalStored) {
                deviceMediaFrames_.fetch_add(copiedFrames,
                                             std::memory_order_relaxed);
            }
        }

        const uint64_t submitted = submittedBefore + availableFrames;
        submittedFrames_.store(submitted, std::memory_order_relaxed);
        devicePaddingFrames_.store(
            std::min<uint64_t>(submitted, paddingFrames + availableFrames),
            std::memory_order_relaxed);
        return true;
    }

    size_t copyFromRingBuffer(float* destination,
                              size_t requestedFrames,
                              bool muted) {
        const uint64_t readCursor = readCursor_.load(std::memory_order_relaxed);
        const uint64_t writeCursor = writeCursor_.load(std::memory_order_acquire);
        const size_t framesToCopy = std::min<size_t>(
            requestedFrames, writeCursor - readCursor);
        const size_t channelCount = static_cast<size_t>(
            formatChannelCount_.load(std::memory_order_relaxed));
        const size_t capacityFrames =
            capacityFrames_.load(std::memory_order_relaxed);
        for (size_t frame = 0; frame < framesToCopy; ++frame) {
            const size_t sourceFrame =
                static_cast<size_t>(readCursor + frame) % capacityFrames;
            const size_t sourceOffset = sourceFrame * channelCount;
            const size_t destinationOffset = frame * channelCount;
            if (muted) {
                std::fill_n(destination + destinationOffset, channelCount, 0.0f);
            } else {
                std::copy_n(samples_.data() + sourceOffset,
                            channelCount,
                            destination + destinationOffset);
            }
        }
        readCursor_.store(readCursor + framesToCopy, std::memory_order_release);
        return framesToCopy;
    }

    void clearRingBuffer() {
        ringResetting_.store(true, std::memory_order_release);
        while (activeWriters_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        readCursor_.store(0, std::memory_order_relaxed);
        writeCursor_.store(0, std::memory_order_relaxed);
        ringResetting_.store(false, std::memory_order_release);
    }

    void updateDevicePadding(IAudioClient* audioClient) {
        UINT32 paddingFrames = 0;
        if (SUCCEEDED(audioClient->GetCurrentPadding(&paddingFrames))) {
            devicePaddingFrames_.store(paddingFrames, std::memory_order_relaxed);
            const uint64_t submitted =
                submittedFrames_.load(std::memory_order_relaxed);
            updatePlayedMediaFrames(
                submitted >= paddingFrames ? submitted - paddingFrames : 0);
        }
    }

    void updatePlayedMediaFrames(uint64_t playedDeviceFrames) {
        uint64_t consumedMediaFrames = 0;
        while (mediaIntervalCount_ > 0) {
            MediaFrameInterval& interval = mediaIntervals_[mediaIntervalStart_];
            if (playedDeviceFrames <= interval.startFrame) {
                break;
            }
            const uint64_t consumedEnd =
                std::min(playedDeviceFrames, interval.endFrame);
            consumedMediaFrames += consumedEnd - interval.startFrame;
            interval.startFrame = consumedEnd;
            if (interval.startFrame < interval.endFrame) {
                break;
            }
            mediaIntervalStart_ =
                (mediaIntervalStart_ + 1) % kMaximumMediaIntervals;
            --mediaIntervalCount_;
        }
        if (consumedMediaFrames == 0) {
            return;
        }
        playedMediaFrames_.fetch_add(consumedMediaFrames,
                                     std::memory_order_relaxed);
        deviceMediaFrames_.fetch_sub(consumedMediaFrames,
                                     std::memory_order_relaxed);
    }

    bool pushMediaInterval(const MediaFrameInterval& interval) {
        if (mediaIntervalCount_ > 0) {
            const size_t last =
                (mediaIntervalStart_ + mediaIntervalCount_ - 1) %
                kMaximumMediaIntervals;
            if (mediaIntervals_[last].endFrame == interval.startFrame) {
                mediaIntervals_[last].endFrame = interval.endFrame;
                return true;
            }
        }
        if (mediaIntervalCount_ == kMaximumMediaIntervals) {
            mediaIntervalOverflow_.store(true, std::memory_order_relaxed);
            return false;
        }
        const size_t destination =
            (mediaIntervalStart_ + mediaIntervalCount_) % kMaximumMediaIntervals;
        mediaIntervals_[destination] = interval;
        ++mediaIntervalCount_;
        return true;
    }

    void finishInitialization(bool succeeded, std::string error) {
        if (!error.empty()) {
            setError(std::move(error));
        }
        {
            std::lock_guard lock(controlMutex_);
            initializationSucceeded_ = succeeded;
            initializationFinished_ = true;
        }
        initializationChanged_.notify_all();
    }

    void finishThread() {
        {
            std::lock_guard lock(controlMutex_);
            threadExited_ = true;
        }
        commandChanged_.notify_all();
    }

    void setError(std::string error) {
        std::lock_guard lock(errorMutex_);
        lastError_ = std::move(error);
    }

    Win32Handle controlEvent_;
    std::jthread worker_;

    std::atomic_int formatSampleRate_{0};
    std::atomic_int formatChannelCount_{0};
    std::vector<float> samples_;
    std::atomic_size_t capacityFrames_{0};
    std::atomic_uint64_t readCursor_{0};
    std::atomic_uint64_t writeCursor_{0};
    std::atomic_bool ringResetting_{false};
    std::atomic_uint32_t activeWriters_{0};

    std::mutex controlMutex_;
    std::condition_variable initializationChanged_;
    std::condition_variable commandChanged_;
    AudioCommand pendingCommand_ = AudioCommand::None;
    uint64_t commandSerial_ = 0;
    uint64_t processedSerial_ = 0;
    bool initializationFinished_ = false;
    bool initializationSucceeded_ = false;
    bool lastCommandSucceeded_ = true;
    bool threadExited_ = true;

    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::atomic_bool muted_{false};
    std::atomic_uint64_t submittedFrames_{0};
    std::atomic_uint64_t devicePaddingFrames_{0};
    std::atomic_uint64_t deviceMediaFrames_{0};
    std::atomic_uint64_t playedMediaFrames_{0};
    std::atomic_bool mediaIntervalOverflow_{false};
    std::array<MediaFrameInterval, kMaximumMediaIntervals> mediaIntervals_{};
    size_t mediaIntervalStart_ = 0;
    size_t mediaIntervalCount_ = 0;
};

}  // namespace

AudioOutputFactory makeWasapiAudioOutputFactory() {
    return [] {
        return std::make_unique<WasapiAudioOutput>();
    };
}

}  // namespace skui::win32
