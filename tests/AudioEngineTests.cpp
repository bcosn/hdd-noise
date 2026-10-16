// Optional native integration test: opens the real default Windows audio device.
// All mix gains are zero. Exit 77 means no usable output device was available.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "AudioEngine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool validSnapshot(const hdd::EngineSnapshot& state) {
    return state.available && state.error.empty() &&
           std::isfinite(state.rpm) && state.rpm >= 0.0 && state.rpm <= 15001.0 &&
           std::isfinite(state.peak) && state.peak >= 0.0;
}

hdd::EngineSnapshot waitForState(hdd::AudioEngine& engine,
                                 hdd::SpindleState wanted,
                                 std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    do {
        const auto state = engine.snapshot();
        require(validSnapshot(state), "audio stream failed or published invalid status");
        if (state.state == wanted) {
            return state;
        }
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error("spindle state transition timed out");
}

void testConcurrentControls(hdd::AudioEngine& engine,
                            const hdd::SynthSettings& silentSettings) {
    std::atomic<bool> stopUpdates{false};
    std::atomic<bool> controlFailed{false};
    std::atomic<unsigned> updateCount{0};
    const auto deadline = Clock::now() + 300ms;
    std::thread controls([&] {
        try {
            auto settings = silentSettings;
            unsigned index = 0;
            while (!stopUpdates.load(std::memory_order_relaxed) &&
                   Clock::now() < deadline) {
                settings.rpm = 5400.0 + static_cast<double>(index % 3) * 900.0;
                settings.impactInterval = 10.0 + index % 6;
                engine.setSettings(settings);
                engine.setActivity(static_cast<double>(index % 101) / 100.0,
                                   static_cast<double>(index % 200));
                if (index % 11 == 0) engine.triggerSeek();
                if (index % 17 == 0) engine.triggerImpact();
                ++index;
                updateCount.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(2ms);
            }
        } catch (...) {
            controlFailed.store(true, std::memory_order_relaxed);
        }
    });

    bool snapshotsValid = true;
    unsigned snapshotCount = 0;
    while (Clock::now() < deadline) {
        if (!validSnapshot(engine.snapshot())) {
            snapshotsValid = false;
            break;
        }
        ++snapshotCount;
        std::this_thread::sleep_for(2ms);
    }
    stopUpdates.store(true, std::memory_order_relaxed);
    controls.join();
    require(!controlFailed.load(), "concurrent control update threw an exception");
    require(snapshotsValid, "concurrent updates corrupted the output state");
    require(updateCount.load() >= 20 && snapshotCount >= 20,
            "concurrent test did not exercise enough updates");
}

void requireStopped(const hdd::AudioEngine& engine) {
    const auto state = engine.snapshot();
    require(!state.available, "closed output must be unavailable");
    require(state.state == hdd::SpindleState::Stopped,
            "closed output must report Stopped");
    require(state.rpm == 0.0 && state.peak == 0.0,
            "closed output must clear RPM and peak");
}

} // namespace

int main() {
    try {
        hdd::AudioEngine engine;
        hdd::SynthSettings settings;
        settings.master = settings.motor = settings.seek = settings.impact = 0.0;
        settings.rpm = 5400.0;
        settings.randomImpacts = false;
        engine.setSettings(settings);
        engine.stop(); // Stopping a never-started engine is also safe.
        requireStopped(engine);

        // This is the application startup order: requests precede device setup.
        engine.setRunning(true);
        if (!engine.start()) {
            std::wcout << L"SKIP AudioEngineTests: no usable default audio output. "
                       << engine.snapshot().error << L'\n';
            return 77;
        }
        const auto running = waitForState(engine, hdd::SpindleState::Running, 4s);
        require(std::abs(running.rpm - settings.rpm) < 5.0,
                "settings or running request was lost before start");

        require(engine.start(), "start on an active device must succeed");
        std::this_thread::sleep_for(100ms);
        const auto stillRunning = engine.snapshot();
        require(validSnapshot(stillRunning) &&
                    stillRunning.state == hdd::SpindleState::Running &&
                    std::abs(stillRunning.rpm - settings.rpm) < 5.0,
                "repeated start reset the running spindle");

        testConcurrentControls(engine, settings);
        engine.setRunning(false);
        const auto stopped = waitForState(engine, hdd::SpindleState::Stopped, 3500ms);
        require(stopped.rpm == 0.0, "completed spin-down must report zero RPM");
        const auto closeStarted = Clock::now();
        engine.stop();
        engine.stop();
        require(Clock::now() - closeStarted < 1500ms,
                "stop did not wake and join the audio worker promptly");
        requireStopped(engine);

        // Warm up the driver once above before checking repeated-device cleanup.
        std::this_thread::sleep_for(50ms);
        DWORD handlesBefore = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE,
                "cannot read initial process handle count");
        for (unsigned cycle = 0; cycle < 10; ++cycle) {
            engine.setRunning(true);
            require(engine.start(), "device could not reopen during lifecycle stress");
            const auto closeTime = Clock::now();
            engine.stop(); // Also covers shutdown before the first buffer is queued.
            require(Clock::now() - closeTime < 1500ms,
                    "immediate stop blocked while the worker was starting");
            requireStopped(engine);
        }
        std::this_thread::sleep_for(50ms);
        DWORD handlesAfter = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE,
                "cannot read final process handle count");
        // Allow a small lazy driver/runtime cache, but catch per-session leaks.
        require(handlesAfter <= handlesBefore + 4,
                "repeated audio sessions leaked process handles");

        std::cout << "AudioEngine integration passed: spin-up, idempotent start, "
                     "concurrent controls, spin-down, repeated stop, 10 rapid "
                     "sessions; handles "
                  << handlesBefore << " -> " << handlesAfter << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL AudioEngineTests: " << error.what() << '\n';
        return 1;
    }
}
