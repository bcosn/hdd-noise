#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include "AudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

namespace hdd {
namespace {

constexpr unsigned kSampleRate = 48000;
constexpr std::size_t kFramesPerBuffer = 480;
constexpr std::size_t kBufferCount = 4;
constexpr std::size_t kChannels = 2;

std::wstring audioError(const wchar_t* action, MMRESULT result) {
    wchar_t description[MAXERRORLENGTH]{};
    waveOutGetErrorTextW(result, description, MAXERRORLENGTH);
    std::wstring message(action);
    message += L"：";
    if (description[0] != L'\0') {
        message += description;
    } else {
        message += L"音频设备错误（";
        message += std::to_wstring(result);
        message += L"）";
    }
    return message;
}

short toPcm(float sample) {
    if (!std::isfinite(sample)) {
        return 0;
    }
    const float limited = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<short>(std::lround(limited * 32767.0f));
}

} // namespace

struct AudioEngine::Impl {
    struct Requests {
        SynthSettings settings;
        std::uint64_t settingsVersion = 1;
        bool running = false;
        double busy = 0.0;
        double tps = 0.0;
        std::uint64_t seekCount = 0;
        std::uint64_t impactCount = 0;
    };

    struct Buffer {
        std::array<short, kFramesPerBuffer * kChannels> samples{};
        WAVEHDR header{};
        bool prepared = false;
        bool submitted = false;
    };

    // The lifecycle mutex never participates in the realtime rendering path.
    std::mutex lifecycleMutex;
    mutable std::mutex dataMutex;
    Requests requests;
    EngineSnapshot status;
    std::array<Buffer, kBufferCount> buffers;
    HWAVEOUT device = nullptr;
    HANDLE playbackEvent = nullptr;
    HANDLE stopEvent = nullptr;
    std::thread worker;
    std::uint64_t consumedSeeks = 0;
    std::uint64_t consumedImpacts = 0;

    void fail(std::wstring message) {
        std::lock_guard<std::mutex> lock(dataMutex);
        status.available = false;
        status.error = std::move(message);
        status.rpm = 0.0;
        status.peak = 0.0;
        status.state = SpindleState::Stopped;
    }

    void closeSession() {
        if (stopEvent != nullptr) {
            SetEvent(stopEvent);
        }
        if (worker.joinable()) {
            worker.join();
        }

        if (device != nullptr) {
            // After the rendering thread has exited, reset returns every queued
            // header before any sample storage or callback handles are released.
            waveOutReset(device);
            for (auto& buffer : buffers) {
                if (buffer.prepared) {
                    waveOutUnprepareHeader(device, &buffer.header,
                                           sizeof(buffer.header));
                }
                buffer.prepared = false;
                buffer.submitted = false;
            }
            waveOutClose(device);
            device = nullptr;
        }
        if (playbackEvent != nullptr) {
            CloseHandle(playbackEvent);
            playbackEvent = nullptr;
        }
        if (stopEvent != nullptr) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }

        std::lock_guard<std::mutex> lock(dataMutex);
        status.available = false;
        status.rpm = 0.0;
        status.peak = 0.0;
        status.state = SpindleState::Stopped;
    }

    void render() noexcept {
        try {
            Synth synth(static_cast<double>(kSampleRate));
            std::uint64_t appliedSettings = 0;
            bool appliedRunning = false;
            bool primed = false;
            const HANDLE events[] = {stopEvent, playbackEvent};

            for (;;) {
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                    return;
                }

                for (auto& buffer : buffers) {
                    if (buffer.submitted &&
                        (buffer.header.dwFlags & WHDR_DONE) == 0) {
                        continue;
                    }
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                        return;
                    }

                    Requests controls;
                    {
                        std::lock_guard<std::mutex> lock(dataMutex);
                        controls = requests;
                    }
                    if (appliedSettings != controls.settingsVersion) {
                        synth.setSettings(controls.settings);
                        appliedSettings = controls.settingsVersion;
                    }
                    if (appliedRunning != controls.running) {
                        synth.setRunning(controls.running);
                        appliedRunning = controls.running;
                    }
                    synth.setActivity(controls.busy, controls.tps);

                    // Consume one audition command per block, so rapid clicks
                    // cannot collapse into a single boolean or overwrite burst.
                    if (consumedSeeks != controls.seekCount) {
                        synth.triggerSeek();
                        ++consumedSeeks;
                    }
                    if (consumedImpacts != controls.impactCount) {
                        synth.triggerImpact();
                        ++consumedImpacts;
                    }

                    for (std::size_t frameIndex = 0;
                         frameIndex < kFramesPerBuffer; ++frameIndex) {
                        const auto frame = synth.next();
                        buffer.samples[frameIndex * kChannels] =
                            toPcm(frame.left);
                        buffer.samples[frameIndex * kChannels + 1] =
                            toPcm(frame.right);
                    }

                    const MMRESULT result = waveOutWrite(
                        device, &buffer.header, sizeof(buffer.header));
                    if (result != MMSYSERR_NOERROR) {
                        fail(audioError(L"无法继续播放合成声音", result));
                        return;
                    }
                    buffer.submitted = true;
                    {
                        std::lock_guard<std::mutex> lock(dataMutex);
                        status.rpm = synth.currentRpm();
                        status.peak = synth.peak();
                        status.state = synth.state();
                    }
                }

                if (!primed) {
                    const MMRESULT result = waveOutRestart(device);
                    if (result != MMSYSERR_NOERROR) {
                        fail(audioError(L"无法启动音频播放", result));
                        return;
                    }
                    primed = true;
                }

                // The first pass primes all four buffers. Thereafter the driver
                // wakes us when a header is reusable; idle playback never polls.
                const DWORD waitResult =
                    WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) {
                    return;
                }
                if (waitResult != WAIT_OBJECT_0 + 1) {
                    fail(L"音频播放等待失败，请重新启动声音引擎。");
                    return;
                }
            }
        } catch (const std::exception&) {
            fail(L"合成声音时发生错误，请重新启动声音引擎。");
        } catch (...) {
            fail(L"合成声音时发生未知错误，请重新启动声音引擎。");
        }
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start() {
    std::lock_guard<std::mutex> lifecycleLock(impl_->lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(impl_->dataMutex);
        if (impl_->status.available) {
            return true;
        }
    }
    impl_->closeSession();
    {
        std::lock_guard<std::mutex> lock(impl_->dataMutex);
        impl_->status.error.clear();
    }

    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->playbackEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->stopEvent == nullptr || impl_->playbackEvent == nullptr) {
        impl_->fail(L"无法创建音频播放事件，请检查系统资源。");
        impl_->closeSession();
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(kChannels);
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign =
        static_cast<WORD>(kChannels * sizeof(short));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    MMRESULT result = waveOutOpen(
        &impl_->device, WAVE_MAPPER, &format,
        reinterpret_cast<DWORD_PTR>(impl_->playbackEvent), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        impl_->device = nullptr;
        impl_->fail(audioError(L"无法打开默认音频设备，请检查扬声器或耳机", result));
        impl_->closeSession();
        return false;
    }

    for (auto& buffer : impl_->buffers) {
        buffer.header = WAVEHDR{};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
        buffer.header.dwBufferLength =
            static_cast<DWORD>(buffer.samples.size() * sizeof(short));
        result = waveOutPrepareHeader(impl_->device, &buffer.header,
                                      sizeof(buffer.header));
        if (result != MMSYSERR_NOERROR) {
            impl_->fail(audioError(L"无法准备音频缓冲区", result));
            impl_->closeSession();
            return false;
        }
        buffer.prepared = true;
        buffer.submitted = false;
    }

    // Hold the device until the worker has queued all four buffers. This avoids
    // starting playback with only the first 10 ms of synthesized audio ready.
    result = waveOutPause(impl_->device);
    if (result != MMSYSERR_NOERROR) {
        impl_->fail(audioError(L"无法初始化音频播放", result));
        impl_->closeSession();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->dataMutex);
        impl_->status.available = true;
    }
    try {
        impl_->worker = std::thread([this] { impl_->render(); });
    } catch (const std::exception&) {
        impl_->fail(L"无法启动音频合成线程，请检查系统资源。");
        impl_->closeSession();
        return false;
    }
    return true;
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> lifecycleLock(impl_->lifecycleMutex);
    impl_->closeSession();
}

void AudioEngine::setSettings(const SynthSettings& settings) {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    impl_->requests.settings = settings;
    ++impl_->requests.settingsVersion;
}

void AudioEngine::setRunning(bool running) {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    impl_->requests.running = running;
}

void AudioEngine::setActivity(double busy01, double tps) {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    impl_->requests.busy = busy01;
    impl_->requests.tps = tps;
}

void AudioEngine::triggerSeek() {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    ++impl_->requests.seekCount;
}

void AudioEngine::triggerImpact() {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    ++impl_->requests.impactCount;
}

EngineSnapshot AudioEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    return impl_->status;
}

} // namespace hdd
