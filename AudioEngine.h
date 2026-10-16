#pragma once

#include "Synth.h"

#include <memory>
#include <string>

namespace hdd {

struct EngineSnapshot {
    double rpm = 0.0;
    double peak = 0.0;
    SpindleState state = SpindleState::Stopped;
    bool available = false;
    std::wstring error;
};

// Owns the output device and the synthesis thread. Public controls are safe to
// call from the UI; the Synth itself is used exclusively by the audio thread.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start();
    void stop();
    void setSettings(const SynthSettings& settings);
    void setRunning(bool running);
    void setActivity(double busy01, double tps);
    void triggerSeek();
    void triggerImpact();
    EngineSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hdd
