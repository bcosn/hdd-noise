#pragma once

#include <array>
#include <cstdint>

namespace hdd {

enum class DriveProfile { WesternDigital = 0, Hitachi = 1, Laptop5400 = 2, Laptop7200 = 3 };

struct SynthSettings {
    double rpm = 7200.0;
    double master = 0.35;
    double motor = 0.55;
    double seek = 0.65;
    double impact = 0.5;
    double impactInterval = 18.0;
    bool randomImpacts = true;
    DriveProfile profile = DriveProfile::WesternDigital;
    bool pulseNoise = false;
    double pulse = 0.30;
    double pulseRate = 8.0;
    double clarity = 0.65;
};

struct SynthFrame {
    float left = 0.0f;
    float right = 0.0f;
};

enum class SpindleState { Stopped, Starting, Running, Stopping };

// A sample-at-a-time, allocation-free procedural sound source. All calls belong
// to the same audio thread. No samples or third-party DSP libraries are used.
class Synth {
public:
    explicit Synth(double sampleRate = 48000.0, std::uint32_t seed = 0x12345678);

    void setSettings(const SynthSettings& settings);
    void setRunning(bool running);
    void setActivity(double busy01, double transfersPerSecond);

    // Manual events are accepted after 65% speed, except while stopping.
    // Seeking auditions a short packet; it does not modify reported disk I/O.
    void triggerSeek();
    void triggerImpact();

    SynthFrame next();
    double currentRpm() const { return currentRpm_; }
    SpindleState state() const { return state_; }
    double peak() const { return peak_; }

    // Monotonic diagnostic counters, also useful for checking load response.
    std::uint64_t seekEventCount() const { return seekEventCount_; }
    std::uint64_t impactEventCount() const { return impactEventCount_; }
    std::uint64_t pulseEventCount() const { return pulseEventCount_; }

private:
    struct Bandpass {
        double b0 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        void configure(double frequency, double q, double sampleRate);
        double process(double input);
        void clear() { z1 = z2 = 0.0; }
    };

    struct MotorBank {
        std::array<Bandpass, 5> bands{};
        std::array<double, 5> gains{};
        double process(double input, double clarity);
        void clear();
    };

    struct CasingBank {
        std::array<Bandpass, 5> left{};
        std::array<Bandpass, 5> right{};
        std::array<double, 5> gains{};
        double process(double input, bool rightChannel, double clarity);
        void clear();
    };

    struct Voice {
        int remaining = 0;
        int age = 0;
        int travel = 1;
        int brakeAt = 0;
        int grainCountdown = 0;
        double strength = 0.0;
        double profile = 0.0;
        std::array<double, 4> weights{};
        double pan = 0.0;
        double polarity = 1.0;
        double attackEnvelope = 1.0;
        double brakeEnvelope = 0.0;
        double grainEnvelope = 0.0;
        bool contact = false;
    };

    double random01();
    double noise();
    double newSeekThreshold();
    Voice& allocateVoice();
    void startSeek(double strength, bool countEvent = true);
    void startContact(double strength, bool countEvent = true);
    double voiceForce(Voice& voice);
    double pulseRandom01();
    double pulseSound();
    void clearPulse();
    void silence();

    SynthSettings settings_{};
    std::array<Voice, 24> voices_{};
    std::array<MotorBank, 4> motorBanks_{};
    std::array<CasingBank, 4> seekBanks_{};
    std::array<CasingBank, 4> contactBanks_{};
    std::array<Bandpass, 2> pulseBands_{};
    double sampleRate_ = 48000.0;
    double inverseSampleRate_ = 1.0 / 48000.0;
    std::uint32_t randomState_ = 0x12345678;
    std::uint32_t pulseRandomState_ = 0x89abcdef;
    bool requestedRunning_ = false;
    SpindleState state_ = SpindleState::Stopped;
    double ramp_ = 0.0;
    double speedFraction_ = 0.0;
    double rpmSetting_ = 7200.0;
    double currentRpm_ = 0.0;
    double rotorPhase_ = 0.0;
    double settingsSmoothing_ = 0.0;
    double rpmSmoothing_ = 0.0;
    double activitySmoothing_ = 0.0;
    double profileSmoothing_ = 0.0;
    double electricalSmoothing_ = 0.0;
    double master_ = 0.35;
    double motor_ = 0.55;
    double seek_ = 0.65;
    double impact_ = 0.5;
    double profile_ = 0.0;
    std::array<double, 4> profileWeights_{{1.0, 0.0, 0.0, 0.0}};
    double clarity_ = 0.65;
    double electrical_ = 0.0;
    double pulseMix_ = 0.0;
    double pulseLevel_ = 0.30;
    double pulseRate_ = 8.0;
    double pulseFadeStep_ = 0.0;
    double pulsePhase_ = 0.0;
    double pulseStrength_ = 1.0;
    double pulseClock_ = 1.0;
    int pulseAge_ = -1;
    double seekRateTarget_ = 0.0;
    double seekRate_ = 0.0;
    double seekClock_ = 0.0;
    double seekThreshold_ = 1.0;
    int seekRefractory_ = 0;
    int burstCountdown_ = 0;
    double burstGain_ = 1.0;
    int auditionRemaining_ = 0;
    int auditionCountdown_ = 0;
    bool startupSettled_ = false;
    int startupSettleSteps_ = 0;
    int startupSettleCountdown_ = 0;
    double impactCountdown_ = 18.0;
    double roughness_ = 0.0;
    double flutter_ = 0.0;
    double sideNoise_ = 0.0;
    double roughnessCoefficient_ = 0.0;
    double flutterCoefficient_ = 0.0;
    double sideCoefficient_ = 0.0;
    double attackDecay_ = 0.0;
    double brakeDecay_ = 0.0;
    double grainDecay_ = 0.0;
    double dcCoefficient_ = 0.0;
    double dcInputLeft_ = 0.0;
    double dcInputRight_ = 0.0;
    double dcOutputLeft_ = 0.0;
    double dcOutputRight_ = 0.0;
    double peak_ = 0.0;
    double peakDecay_ = 0.0;
    std::uint64_t seekEventCount_ = 0;
    std::uint64_t impactEventCount_ = 0;
    std::uint64_t pulseEventCount_ = 0;
};

} // namespace hdd
