#include "Synth.h"

#include <algorithm>
#include <cmath>

namespace hdd {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

double safeClamp(double value, double low, double high, double fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

double smoothstep(double value) {
    return value * value * (3.0 - 2.0 * value);
}

void smoothGain(double& current, double target, double smoothing) {
    current += (target - current) * smoothing;
    if (std::abs(current - target) < 1.0e-10) {
        current = target;
    }
}

double coefficient(double frequency, double sampleRate) {
    return 1.0 - std::exp(-kTwoPi * frequency / sampleRate);
}
} // namespace

void Synth::Bandpass::configure(double frequency, double q, double sampleRate) {
    const double angle = kTwoPi * std::min(frequency, sampleRate * 0.43) / sampleRate;
    const double alpha = std::sin(angle) / (2.0 * q);
    const double denominator = 1.0 + alpha;
    b0 = alpha / denominator;
    b2 = -b0;
    a1 = -2.0 * std::cos(angle) / denominator;
    a2 = (1.0 - alpha) / denominator;
}

double Synth::Bandpass::process(double input) {
    const double output = b0 * input + z1;
    z1 = -a1 * output + z2;
    z2 = b2 * input - a2 * output;
    if (std::abs(z1) + std::abs(z2) < 1.0e-20) {
        z1 = z2 = 0.0;
    }
    return output;
}

double Synth::MotorBank::process(double input, double clarity) {
    double output = 0.0;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const double tone = i == 0 ? 1.0 - 0.60 * clarity :
            i == 1 ? 1.0 - 0.20 * clarity : 1.0 + (0.12 + 0.13 * (i - 2)) * clarity;
        output += bands[i].process(input) * gains[i] * tone;
    }
    return output;
}

void Synth::MotorBank::clear() {
    for (auto& band : bands) {
        band.clear();
    }
}

double Synth::CasingBank::process(double input, bool rightChannel, double clarity) {
    auto& bands = rightChannel ? right : left;
    double output = 0.0;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const double tone = i == 0 ? 1.0 - 0.45 * clarity :
            i == 1 ? 1.0 - 0.10 * clarity : 1.0 + (0.10 + 0.15 * (i - 2)) * clarity;
        output += bands[i].process(input) * gains[i] * tone;
    }
    return output;
}

void Synth::CasingBank::clear() {
    for (auto& band : left) {
        band.clear();
    }
    for (auto& band : right) {
        band.clear();
    }
}

Synth::Synth(double sampleRate, std::uint32_t seed)
    : sampleRate_(safeClamp(sampleRate, 8000.0, 192000.0, 48000.0)),
      inverseSampleRate_(1.0 / sampleRate_),
      randomState_(seed ? seed : 0x12345678) {
    settingsSmoothing_ = 1.0 - std::exp(-1.0 / (0.018 * sampleRate_));
    rpmSmoothing_ = 1.0 - std::exp(-1.0 / (0.22 * sampleRate_));
    activitySmoothing_ = 1.0 - std::exp(-1.0 / (0.075 * sampleRate_));
    profileSmoothing_ = 1.0 - std::exp(-1.0 / (0.11 * sampleRate_));
    electricalSmoothing_ = 1.0 - std::exp(-1.0 / (0.035 * sampleRate_));
    roughnessCoefficient_ = coefficient(115.0, sampleRate_);
    flutterCoefficient_ = coefficient(9.0, sampleRate_);
    sideCoefficient_ = coefficient(1100.0, sampleRate_);
    attackDecay_ = std::exp(-1.0 / (0.0008 * sampleRate_));
    brakeDecay_ = std::exp(-1.0 / (0.0012 * sampleRate_));
    grainDecay_ = std::exp(-1.0 / (0.00028 * sampleRate_));
    dcCoefficient_ = std::exp(-kTwoPi * 20.0 / sampleRate_);
    peakDecay_ = std::exp(-1.0 / (0.28 * sampleRate_));
    pulseRandomState_ = randomState_ ^ 0xa511e9b3u;
    if (pulseRandomState_ == 0) pulseRandomState_ = 0x89abcdefu;
    pulseFadeStep_ = 1.0 / (0.030 * sampleRate_);
    pulseBands_[0].configure(1650.0, 2.0, sampleRate_);
    pulseBands_[1].configure(3900.0, 2.5, sampleRate_);

    // Reference-inspired desktop timbres and two explicit small-chassis models.
    // Notebook profiles use less low body, shorter/lighter actuator strokes,
    // and different shell resonances, rather than merely changing motor RPM.
    constexpr double motorFrequencies[4][5] = {
        {194,630,1140,2880,5884}, {237,592,1157,2283,6200},
        {280,850,1800,3240,5400}, {340,1080,2160,4320,6800}
    };
    constexpr double motorQ[4][5] = {
        {2.2,1.2,2.2,2.7,2.0}, {2.3,1.2,1.8,2.5,2.0},
        {2.0,1.3,2.1,2.5,1.8}, {2.0,1.4,2.0,2.4,1.9}
    };
    motorBanks_[0].gains = {1.65,0.10,0.43,0.23,0.028};
    motorBanks_[1].gains = {1.55,0.18,0.40,0.20,0.035};
    motorBanks_[2].gains = {0.68,0.14,0.24,0.16,0.022};
    motorBanks_[3].gains = {0.63,0.15,0.25,0.17,0.028};
    constexpr double seekFrequencies[4][5] = {
        {350,920,2100,4500,7300}, {590,1450,2850,5500,7300},
        {900,1800,3700,6500,9500}, {1080,2100,4100,6900,10200}
    };
    constexpr double seekQ[4][5] = {
        {1.05,1.0,0.85,1.2,1.4}, {0.9,0.8,0.9,1.15,1.4},
        {1.0,1.2,1.1,1.5,1.7}, {1.0,1.2,1.2,1.5,1.7}
    };
    seekBanks_[0].gains = {0.55,0.52,0.48,0.30,0.13};
    seekBanks_[1].gains = {0.24,0.47,0.67,0.57,0.24};
    seekBanks_[2].gains = {0.20,0.40,0.38,0.16,0.07};
    seekBanks_[3].gains = {0.22,0.43,0.40,0.19,0.08};

    // A metal stop has an inharmonic, brief ring. It must not reuse the broad
    // low-Q seek filters: that was responsible for the old hollow/plastic thud.
    constexpr double contactFrequencies[4][5] = {
        {420,1380,2960,5230,8100}, {530,1670,3370,6100,9250},
        {800,2100,4600,7300,10800}, {980,2450,4800,8100,11600}
    };
    constexpr double contactQ[4][5] = {
        {4,18,45,80,110}, {4,19,46,82,110},
        {4,16,35,60,80}, {4,17,38,65,85}
    };
    contactBanks_[0].gains = {0.70,1.50,2.10,1.70,0.85};
    contactBanks_[1].gains = {0.65,1.45,2.00,1.75,0.90};
    contactBanks_[2].gains = {0.28,0.95,1.40,1.15,0.55};
    contactBanks_[3].gains = {0.30,1.00,1.50,1.20,0.60};
    for (std::size_t profile = 0; profile < motorBanks_.size(); ++profile) {
        for (std::size_t i = 0; i < 5; ++i) {
            motorBanks_[profile].bands[i].configure(motorFrequencies[profile][i],motorQ[profile][i],sampleRate_);
            for (auto* band : {&seekBanks_[profile].left[i],&seekBanks_[profile].right[i]})
                band->configure(seekFrequencies[profile][i],seekQ[profile][i],sampleRate_);
            for (auto* band : {&contactBanks_[profile].left[i],&contactBanks_[profile].right[i]})
                band->configure(contactFrequencies[profile][i],contactQ[profile][i],sampleRate_);
        }
    }
    seekThreshold_ = newSeekThreshold();
    impactCountdown_ = settings_.impactInterval * (0.5 + random01());
}

void Synth::setSettings(const SynthSettings& settings) {
    const double previousInterval = settings_.impactInterval;
    settings_.rpm = safeClamp(settings.rpm, 600.0, 15000.0, 7200.0);
    settings_.master = safeClamp(settings.master, 0.0, 1.0, 0.35);
    settings_.motor = safeClamp(settings.motor, 0.0, 1.0, 0.55);
    settings_.seek = safeClamp(settings.seek, 0.0, 1.0, 0.65);
    settings_.impact = safeClamp(settings.impact, 0.0, 1.0, 0.5);
    settings_.impactInterval = safeClamp(settings.impactInterval, 0.5, 600.0, 18.0);
    settings_.randomImpacts = settings.randomImpacts;
    const int profile = static_cast<int>(settings.profile);
    settings_.profile = profile >= 0 && profile < 4 ? settings.profile : DriveProfile::WesternDigital;
    settings_.pulseNoise = settings.pulseNoise;
    settings_.pulse = safeClamp(settings.pulse, 0.0, 1.0, 0.30);
    settings_.pulseRate = safeClamp(settings.pulseRate, 2.0, 40.0, 8.0);
    settings_.clarity = safeClamp(settings.clarity, 0.0, 1.0, 0.65);
    impactCountdown_ *= settings_.impactInterval / previousInterval;
}

void Synth::setRunning(bool running) {
    if (!running && requestedRunning_ && speedFraction_ > 0.20) {
        // A short park contact precedes the unpowered spindle coast.
        startContact(0.62, false);
    }
    if (running && !requestedRunning_ && ramp_ == 0.0) {
        startupSettled_ = false;
    }
    requestedRunning_ = running;
    if (running) {
        state_ = ramp_ >= 1.0 ? SpindleState::Running : SpindleState::Starting;
    } else {
        state_ = ramp_ <= 0.0 ? SpindleState::Stopped : SpindleState::Stopping;
        auditionRemaining_ = auditionCountdown_ = 0;
        startupSettleSteps_ = 0;
    }
}

void Synth::setActivity(double busy01, double transfersPerSecond) {
    const double busy = safeClamp(busy01, 0.0, 1.0, 0.0);
    const double transfers = safeClamp(transfersPerSecond, 0.0, 1000000.0, 0.0);
    // IOPS dominates actuator activity; long sequential transfers may still
    // report high busy percentage without continual large head movements.
    seekRateTarget_ = std::min(110.0,
        std::max(0.0, busy - 0.012) * 18.0 + std::sqrt(transfers) * 3.0);
}

double Synth::random01() {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return (static_cast<double>(randomState_) + 0.5) / 4294967296.0;
}

double Synth::noise() {
    return random01() * 2.0 - 1.0;
}

double Synth::newSeekThreshold() {
    return 0.24 - std::log(random01());
}

Synth::Voice& Synth::allocateVoice() {
    Voice* earliest = &voices_.front();
    for (auto& voice : voices_) {
        if (voice.remaining == 0) {
            return voice;
        }
        if (voice.remaining < earliest->remaining) {
            earliest = &voice;
        }
    }
    return *earliest;
}

void Synth::startSeek(double strength, bool countEvent) {
    auto& voice = allocateVoice();
    voice = Voice{};
    voice.profile = profile_;
    voice.weights = profileWeights_;
    const double smallDrive = profileWeights_[2] + profileWeights_[3];
    voice.travel = std::max(1, static_cast<int>(sampleRate_ *
        (0.0033 + random01() * (0.0067 + profile_ * 0.003)) * (1.0 - 0.40 * smallDrive)));
    voice.remaining = voice.travel + static_cast<int>(sampleRate_ * 0.012);
    voice.brakeAt = static_cast<int>(voice.travel * (0.54 + random01() * 0.16));
    voice.strength = strength * (0.74 + random01() * 0.46) * (1.0 - 0.40 * smallDrive);
    voice.pan = noise() * 0.09;
    voice.polarity = random01() < 0.5 ? -1.0 : 1.0;
    voice.grainCountdown = 1;
    if (countEvent) {
        ++seekEventCount_;
    }
}

void Synth::startContact(double strength, bool countEvent) {
    auto& voice = allocateVoice();
    voice = Voice{};
    voice.contact = true;
    voice.profile = profile_;
    voice.weights = profileWeights_;
    const double smallDrive = profileWeights_[2] + profileWeights_[3];
    voice.travel = std::max(2, static_cast<int>(sampleRate_ * (0.00018 + random01() * 0.00010)));
    voice.brakeAt = static_cast<int>(sampleRate_ * (0.0038 + random01() * 0.0025));
    voice.remaining = static_cast<int>(sampleRate_ * 0.050);
    voice.strength = strength * (0.90 + random01() * 0.20) * (1.0 - 0.25 * smallDrive);
    voice.pan = noise() * 0.04;
    voice.polarity = random01() < 0.5 ? -1.0 : 1.0;
    if (countEvent) {
        ++impactEventCount_;
    }
}

double Synth::voiceForce(Voice& voice) {
    if (voice.contact) {
        // Sub-millisecond contact and weaker rebound excite the same metal
        // modes on every hit; a long low-frequency drive-current thud is absent.
        double impulse = 0.0;
        if (voice.age < voice.travel)
            impulse = std::sin(kPi * (voice.age + 0.5) / voice.travel);
        const int rebound = voice.age - voice.brakeAt;
        if (rebound >= 0 && rebound < voice.travel)
            impulse += 0.24 * std::sin(kPi * (rebound + 0.5) / voice.travel);
        const double scrape = noise() * 0.08 * std::exp(-voice.age * inverseSampleRate_ / 0.00045);
        ++voice.age; --voice.remaining;
        return (impulse + scrape) * voice.strength * voice.polarity * 3.2;
    }
    if (voice.age == voice.brakeAt) {
        voice.brakeEnvelope = voice.contact ? 0.46 : 0.83;
    }
    double current = 0.0;
    double travelEnvelope = 0.0;
    if (voice.age < voice.travel) {
        const double position = static_cast<double>(voice.age) / voice.travel;
        // Finite bipolar VCM current, never a free-running seek oscillator.
        current = std::sin(kTwoPi * position) * voice.polarity;
        travelEnvelope = 0.5 - 0.5 * std::cos(kTwoPi * position);
        if (!voice.contact && --voice.grainCountdown <= 0) {
            voice.grainEnvelope += 0.45 + random01() * 0.55;
            voice.grainCountdown = std::max(1, static_cast<int>(sampleRate_ *
                (0.00045 + random01() * 0.0013)));
        }
    }
    const double startRamp = std::min(1.0,
        voice.age * inverseSampleRate_ / 0.00012);
    const double edges = voice.attackEnvelope + voice.brakeEnvelope;
    const double scratch = voice.profile * voice.grainEnvelope * travelEnvelope;
    const double broadband = noise() *
        (0.72 * edges + 0.22 * travelEnvelope + 0.66 * scratch);
    const double force = voice.strength * startRamp *
        (broadband + current * (voice.contact ? 0.48 : 0.66));
    voice.attackEnvelope *= attackDecay_;
    voice.brakeEnvelope *= brakeDecay_;
    voice.grainEnvelope *= grainDecay_;
    ++voice.age;
    --voice.remaining;
    return force;
}

void Synth::triggerSeek() {
    if (requestedRunning_ && speedFraction_ >= 0.65) {
        startSeek(1.0);
        auditionRemaining_ = static_cast<int>(sampleRate_ * 0.31);
        auditionCountdown_ = static_cast<int>(sampleRate_ * 0.016);
    }
}

void Synth::triggerImpact() {
    if (requestedRunning_ && speedFraction_ >= 0.65) {
        startContact(1.0);
    }
}

double Synth::pulseRandom01() {
    pulseRandomState_ ^= pulseRandomState_ << 13;
    pulseRandomState_ ^= pulseRandomState_ >> 17;
    pulseRandomState_ ^= pulseRandomState_ << 5;
    return (static_cast<double>(pulseRandomState_) + 0.5) / 4294967296.0;
}

double Synth::pulseSound() {
    // Deliberately discrete impulses, not a bed of electrical hiss. A complete
    // pulse lasts 9 ms; the rest of each 2--40 Hz period is exactly quiet.
    pulseClock_ += pulseRate_ * inverseSampleRate_;
    if (pulseClock_ >= 1.0) {
        pulseClock_ -= std::floor(pulseClock_);
        pulseAge_ = 0; pulsePhase_ = 0.0;
        pulseStrength_ = 0.90 + pulseRandom01() * 0.10;
        for (auto& band : pulseBands_) band.clear();
        ++pulseEventCount_;
    }
    if (pulseAge_ < 0) return 0.0;
    const double t = pulseAge_ * inverseSampleRate_;
    if (t >= 0.009) { pulseAge_ = -1; return 0.0; }
    const double attack = std::min(1.0,t/0.00015);
    const double tail = std::min(1.0,(0.009-t)/0.001);
    const double envelope = attack * tail * std::exp(-t/0.0022);
    const double smallDrive = profileWeights_[2] + profileWeights_[3];
    const double frequency = std::min(1750.0 + 260.0 * profile_ + 500.0 * smallDrive,sampleRate_*0.35);
    const double harmonicRoom=std::clamp((sampleRate_*0.46-frequency*2.17)/(sampleRate_*0.08),0.0,1.0);
    pulsePhase_ += frequency * inverseSampleRate_;
    const double white = pulseRandom01() * 2.0 - 1.0;
    const double texture = pulseBands_[0].process(white)*0.08 + pulseBands_[1].process(white)*0.05;
    const double carrier = std::sin(kTwoPi*pulsePhase_) +
        std::sin(kTwoPi*pulsePhase_*2.17)*0.21*harmonicRoom;
    ++pulseAge_;
    return (carrier*0.48+texture)*envelope*pulseStrength_*pulseMix_*pulseLevel_*electrical_;
}

void Synth::clearPulse() {
    for (auto& band : pulseBands_) band.clear();
    pulseAge_ = -1; pulsePhase_ = 0.0; pulseClock_ = 1.0;
}

void Synth::silence() {
    for (auto& voice : voices_) {
        voice.remaining = 0;
    }
    for (auto& bank : motorBanks_) {
        bank.clear();
    }
    for (auto& bank : seekBanks_) {
        bank.clear();
    }
    for (auto& bank : contactBanks_) {
        bank.clear();
    }
    currentRpm_ = speedFraction_ = electrical_ = 0.0;
    auditionRemaining_ = auditionCountdown_ = 0;
    startupSettleSteps_ = startupSettleCountdown_ = 0;
    seekRefractory_ = 0;
    seekClock_ = 0.0;
    roughness_ = flutter_ = sideNoise_ = 0.0;
    dcInputLeft_ = dcInputRight_ = 0.0;
    dcOutputLeft_ = dcOutputRight_ = 0.0;
    clearPulse();
}

SynthFrame Synth::next() {
    smoothGain(master_, settings_.master, settingsSmoothing_);
    smoothGain(motor_, settings_.motor, settingsSmoothing_);
    smoothGain(seek_, settings_.seek, settingsSmoothing_);
    smoothGain(impact_, settings_.impact, settingsSmoothing_);
    for (std::size_t i=0;i<profileWeights_.size();++i)
        smoothGain(profileWeights_[i],i==static_cast<std::size_t>(settings_.profile)?1.0:0.0,profileSmoothing_);
    profile_=profileWeights_[1]+0.35*profileWeights_[2]+0.55*profileWeights_[3];
    smoothGain(clarity_,settings_.clarity,settingsSmoothing_);
    smoothGain(electrical_, requestedRunning_ ? 1.0 : 0.0, electricalSmoothing_);
    smoothGain(pulseLevel_,settings_.pulse,settingsSmoothing_);
    smoothGain(pulseRate_,settings_.pulseRate,settingsSmoothing_);
    const double pulseTarget = settings_.pulseNoise ? 1.0 : 0.0;
    pulseMix_ += std::clamp(pulseTarget-pulseMix_,-pulseFadeStep_,pulseFadeStep_);
    rpmSetting_ += (settings_.rpm - rpmSetting_) * rpmSmoothing_;
    seekRate_ += (seekRateTarget_ - seekRate_) * activitySmoothing_;
    if (seekRateTarget_ == 0.0 && seekRate_ < 1.0e-9) {
        seekRate_ = 0.0;
    }
    peak_ *= peakDecay_;
    if (peak_ < 1.0e-10) {
        peak_ = 0.0;
    }

    if (requestedRunning_) {
        ramp_ = std::min(1.0, ramp_ + inverseSampleRate_ / 3.0);
        state_ = ramp_ >= 1.0 ? SpindleState::Running : SpindleState::Starting;
    } else if (ramp_ > 0.0) {
        ramp_ = std::max(0.0, ramp_ - inverseSampleRate_ / 2.4);
        state_ = ramp_ > 0.0 ? SpindleState::Stopping : SpindleState::Stopped;
    }
    if (ramp_ <= 0.0) {
        if (currentRpm_ != 0.0) {
            silence();
        }
        return {};
    }

    speedFraction_ = smoothstep(ramp_);
    currentRpm_ = rpmSetting_ * speedFraction_;
    const double white = noise();
    roughness_ += roughnessCoefficient_ * (white - roughness_);
    flutter_ += flutterCoefficient_ * (noise() - flutter_);
    const double fundamental = currentRpm_ / 60.0;
    rotorPhase_ += fundamental * (1.0 + roughness_ * 0.0013) * inverseSampleRate_;
    rotorPhase_ -= std::floor(rotorPhase_);
    const double angle = kTwoPi * rotorPhase_;
    const double gate = std::sqrt(speedFraction_);

    // Keep banks warm so changing chassis presets crossfades continuously.
    double motorNoise=0.0;
    for (std::size_t i=0;i<motorBanks_.size();++i)
        motorNoise+=motorBanks_[i].process(white,clarity_)*profileWeights_[i];
    const double smallDrive=profileWeights_[2]+profileWeights_[3];
    const double rotationalTone=(0.085-0.042*smallDrive)*(1.0-0.30*clarity_)*std::sin(angle)+
        0.006*std::sin(angle*2.0);
    double electricalTone=0.0;
    constexpr int orders[6]={6,12,18,24,36,48};
    constexpr double levels[4][6]={
        {0.002,0.004,0.003,0.025,0.004,0.007},
        {0.003,0.023,0.005,0.004,0.009,0.002},
        {0.001,0.003,0.016,0.003,0.011,0.002},
        {0.001,0.004,0.020,0.003,0.010,0.002}
    };
    for(std::size_t i=0;i<6;++i){
        const double room=std::clamp((sampleRate_*0.46-fundamental*orders[i])/(sampleRate_*0.08),0.0,1.0);
        double level=0.0;
        for(std::size_t j=0;j<4;++j)level+=levels[j][i]*profileWeights_[j];
        electricalTone+=level*room*std::sin(angle*orders[i]+roughness_*0.06);
    }
    // Electrical forces fade after power-off while mechanical rotation coasts.
    const double spinWhine = electricalTone * electrical_ *
        (1.0 + 0.8 * (1.0 - speedFraction_));
    const double roughGain = 1.0 + 0.08 * std::sin(angle) + flutter_ * 0.8;
    const double rotation = motor_ *
        ((motorNoise * roughGain + rotationalTone) * (0.20 + 0.80 * speedFraction_) +
         spinWhine);
    const double sideWhite = noise();
    sideNoise_ += sideCoefficient_ * (sideWhite - sideNoise_);
    const double width = motor_ * speedFraction_ * (sideWhite - sideNoise_) * 0.002;
    double left = rotation + width;
    double right = rotation - width;

    if (requestedRunning_ && speedFraction_ >= 0.8) {
        if (--burstCountdown_ <= 0) {
            burstCountdown_ = static_cast<int>(sampleRate_ * (0.025 + random01() * 0.070));
            burstGain_ = random01() < 0.22 ? 0.20 : 0.90 + random01() * 0.65;
        }
        if (seekRefractory_ > 0) {
            --seekRefractory_;
        }
        seekClock_ += seekRate_ * burstGain_ * inverseSampleRate_;
        if (seekRefractory_ == 0 && seekClock_ >= seekThreshold_) {
            seekClock_ = 0.0;
            seekThreshold_ = newSeekThreshold();
            startSeek(0.68 + random01() * 0.36);
            seekRefractory_ = static_cast<int>(sampleRate_ * 0.003);
        }
        if (settings_.randomImpacts) {
            impactCountdown_ -= inverseSampleRate_;
            if (impactCountdown_ <= 0.0) {
                startContact(0.85);
                impactCountdown_ = settings_.impactInterval * (0.5 + random01());
            }
        }
    }
    if (requestedRunning_ && auditionRemaining_ > 0) {
        --auditionRemaining_;
        if (--auditionCountdown_ <= 0) {
            startSeek(0.62 + random01() * 0.46);
            auditionCountdown_ = std::max(1, static_cast<int>(sampleRate_ *
                (0.009 + random01() * 0.021)));
        }
    }
    if (requestedRunning_ && !startupSettled_ && speedFraction_ >= 0.965) {
        startupSettled_ = true;
        startContact(0.36, false);
        startupSettleSteps_ = 2;
        startupSettleCountdown_ = static_cast<int>(sampleRate_ * 0.065);
    }
    if (requestedRunning_ && startupSettleSteps_ > 0 && --startupSettleCountdown_ <= 0) {
        startSeek(0.32, false);
        --startupSettleSteps_;
        startupSettleCountdown_ = static_cast<int>(sampleRate_ * 0.037);
    }

    std::array<double, 4> seekLeft{}, seekRight{}, contactLeft{}, contactRight{};
    for (auto& voice : voices_) {
        if (voice.remaining == 0) {
            continue;
        }
        const double force = voiceForce(voice);
        auto& forceLeft = voice.contact ? contactLeft : seekLeft;
        auto& forceRight = voice.contact ? contactRight : seekRight;
        for(std::size_t i=0;i<4;++i){
            forceLeft[i]+=force*(1.0-voice.pan)*voice.weights[i];
            forceRight[i]+=force*(1.0+voice.pan)*voice.weights[i];
        }
    }
    for (std::size_t profile = 0; profile < seekBanks_.size(); ++profile) {
        left += seekBanks_[profile].process(seekLeft[profile], false, clarity_) * seek_ * 1.45;
        right += seekBanks_[profile].process(seekRight[profile], true, clarity_) * seek_ * 1.45;
        left += contactBanks_[profile].process(contactLeft[profile], false, clarity_) * impact_ * 1.50;
        right += contactBanks_[profile].process(contactRight[profile], true, clarity_) * impact_ * 1.50;
    }

    if(pulseMix_>0.0){const double pulse=pulseSound();left+=pulse;right+=pulse;}
    else if(pulseAge_>=0)clearPulse();

    left *= master_ * gate;
    right *= master_ * gate;
    const double dcLeft = left - dcInputLeft_ + dcCoefficient_ * dcOutputLeft_;
    const double dcRight = right - dcInputRight_ + dcCoefficient_ * dcOutputRight_;
    dcInputLeft_ = left;
    dcInputRight_ = right;
    dcOutputLeft_ = std::abs(dcLeft) < 1.0e-20 ? 0.0 : dcLeft;
    dcOutputRight_ = std::abs(dcRight) < 1.0e-20 ? 0.0 : dcRight;
    // Preserve normal-level transients. Only peaks above 0.8 enter the limiter;
    // the former always-on saturation dulled even quiet mechanical detail.
    auto limit=[](double value){const double a=std::abs(value);return a<=0.8?value:
        std::copysign(0.8+0.1999*(1.0-std::exp(-(a-0.8)/0.1999)),value);};
    const double outLeft=limit(dcLeft),outRight=limit(dcRight);
    peak_ = std::max(peak_, std::max(std::abs(outLeft), std::abs(outRight)));
    return {static_cast<float>(outLeft), static_cast<float>(outRight)};
}

} // namespace hdd
