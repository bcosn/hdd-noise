#include "Synth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void advance(hdd::Synth& synth, double seconds) {
    const int count = static_cast<int>(seconds * kSampleRate);
    for (int i = 0; i < count; ++i) {
        synth.next();
    }
}

void testSpindleAndSilence() {
    hdd::Synth synth;
    require(synth.state() == hdd::SpindleState::Stopped, "initial spindle state");
    for (int i = 0; i < 2000; ++i) {
        const auto sample = synth.next();
        require(sample.left == 0.0f && sample.right == 0.0f, "initial exact silence");
    }
    synth.setRunning(true);
    advance(synth, 0.6);
    const double earlyRpm = synth.currentRpm();
    advance(synth, 1.0);
    require(synth.currentRpm() > earlyRpm * 2.0, "spin-up must raise pitch/RPM");
    advance(synth, 1.5);
    require(synth.state() == hdd::SpindleState::Running, "spin-up completes in 3 s");
    require(std::abs(synth.currentRpm() - 7200.0) < 0.01, "requested running RPM");

    synth.triggerImpact();
    synth.triggerSeek();
    synth.setRunning(false);
    advance(synth, 0.5);
    require(synth.currentRpm() < 7200.0, "spin-down lowers RPM");
    advance(synth, 2.0);
    require(synth.state() == hdd::SpindleState::Stopped, "spin-down completes in 2.4 s");
    for (int i = 0; i < 10000; ++i) {
        const auto sample = synth.next();
        require(sample.left == 0.0f && sample.right == 0.0f, "stopped exact silence");
    }
    require(synth.currentRpm() == 0.0, "stopped RPM is zero");
}

void testReversal() {
    hdd::Synth synth;
    synth.setRunning(true);
    advance(synth, 3.1);
    synth.setRunning(false);
    advance(synth, 0.8);
    const double before = synth.currentRpm();
    synth.setRunning(true);
    const auto sample = synth.next();
    require(std::isfinite(sample.left), "reversal finite sample");
    require(std::abs(synth.currentRpm() - before) < 1.0, "reversal preserves speed");
    advance(synth, 0.3);
    require(synth.currentRpm() > before, "reversal accelerates again");
    advance(synth, 1.0);
    require(synth.state() == hdd::SpindleState::Running, "reversal reaches running");
}

void testDeterministicSeed() {
    hdd::Synth first(kSampleRate, 4127), second(kSampleRate, 4127);
    first.setRunning(true);
    second.setRunning(true);
    first.setActivity(0.9, 220.0);
    second.setActivity(0.9, 220.0);
    for (int i = 0; i < 300000; ++i) {
        if (i == 180000) {
            first.triggerImpact();
            second.triggerImpact();
        }
        const auto a = first.next();
        const auto b = second.next();
        require(a.left == b.left && a.right == b.right, "seed reproducibility");
    }
    require(first.seekEventCount() > 0, "determinism includes automatic seeks");
}

void testActivityResponseAndManualEvents() {
    hdd::Synth quiet, busy;
    hdd::SynthSettings settings;
    settings.randomImpacts = false;
    quiet.setSettings(settings);
    busy.setSettings(settings);
    quiet.setRunning(true);
    busy.setRunning(true);
    busy.setActivity(0.98, 640.0);
    advance(quiet, 7.0);
    advance(busy, 7.0);
    require(quiet.seekEventCount() == 0, "idle disk has no automatic seeks");
    require(busy.seekEventCount() > 150, "busy disk has a dense seek pattern");
    require(quiet.impactEventCount() == 0, "random impacts can be disabled");
    quiet.triggerSeek();
    quiet.triggerImpact();
    require(quiet.seekEventCount() == 1 && quiet.impactEventCount() == 1,
            "manual transients accepted while running");
    quiet.setRunning(false);
    quiet.triggerSeek();
    quiet.triggerImpact();
    require(quiet.seekEventCount() == 1 && quiet.impactEventCount() == 1,
            "manual transients ignored while stopping");
    quiet.setRunning(true);
    settings.randomImpacts = true;
    settings.impactInterval = 0.5;
    quiet.setSettings(settings);
    advance(quiet, 2.0);
    require(quiet.impactEventCount() >= 3,
            "enabled random impacts occur at the configured interval");
}

void testBoundedExtremeMix() {
    hdd::Synth synth(8000.0, 51);
    hdd::SynthSettings settings;
    settings.rpm = 15000.0;
    settings.master = settings.motor = settings.seek = settings.impact = 1.0;
    settings.pulseNoise = true;
    settings.pulse = 1.0;
    settings.impactInterval = 0.5;
    synth.setSettings(settings);
    synth.setActivity(1.0, 1000000.0);
    synth.setRunning(true);
    double largest = 0.0;
    for (int i = 0; i < 160000; ++i) {
        if (i % 173 == 0) {
            synth.triggerSeek();
            synth.triggerImpact();
        }
        const auto sample = synth.next();
        require(std::isfinite(sample.left) && std::isfinite(sample.right),
                "extreme mix must remain finite");
        largest = std::max(largest, std::max(std::abs(static_cast<double>(sample.left)),
                                           std::abs(static_cast<double>(sample.right))));
        require(std::abs(sample.left) < 1.0f && std::abs(sample.right) < 1.0f,
                "extreme mix must not clip");
    }
    require(largest > 0.1 && synth.peak() < 1.0, "nontrivial bounded output/meter");
    require(synth.impactEventCount() > 0, "random and manual impact path exercised");
}

double frequencyEnergy(hdd::Synth& synth, double frequency, double otherFrequency) {
    double real = 0.0, imaginary = 0.0, otherReal = 0.0, otherImaginary = 0.0;
    for (int i = 0; i < static_cast<int>(kSampleRate); ++i) {
        const auto sample = synth.next();
        const double phase = kTwoPi * i / kSampleRate;
        real += sample.left * std::cos(phase * frequency);
        imaginary += sample.left * std::sin(phase * frequency);
        otherReal += sample.left * std::cos(phase * otherFrequency);
        otherImaginary += sample.left * std::sin(phase * otherFrequency);
    }
    return (real * real + imaginary * imaginary) /
           std::max(1.0e-12, otherReal * otherReal + otherImaginary * otherImaginary);
}

void testRpmControlsElectricalOrders() {
    for (const double rpm : {5400.0, 7200.0}) {
        hdd::Synth synth;
        hdd::SynthSettings settings;
        settings.rpm = rpm;
        settings.motor = 1.0;
        settings.seek = settings.impact = 0.0;
        settings.randomImpacts = false;
        synth.setSettings(settings);
        synth.setRunning(true);
        advance(synth, 4.0);
        const double frequency = rpm / 60.0 * 24.0;
        const double other = rpm == 5400.0 ? 2880.0 : 2160.0;
        require(frequencyEnergy(synth, frequency, other) > 12.0,
                "electrical motor orders follow RPM / 60");
    }
}

void testMotorIsBroadbandAndCentered() {
    for (const auto profile : {hdd::DriveProfile::WesternDigital, hdd::DriveProfile::Hitachi}) {
        hdd::Synth synth;
        hdd::SynthSettings settings;
        settings.master = 0.5;
        settings.motor = 1.0;
        settings.seek = settings.impact = 0.0;
        settings.randomImpacts = false;
        settings.profile = profile;
        synth.setSettings(settings);
        synth.setRunning(true);
        advance(synth, 4.0);
        double energy = 0.0, highEnergy = 0.0, sideEnergy = 0.0;
        double real = 0.0, imaginary = 0.0;
        double low[3]{};
        const double smoothing = 1.0 - std::exp(-kTwoPi * 800.0 / kSampleRate);
        for (int i = 0; i < static_cast<int>(kSampleRate); ++i) {
            const auto sample = synth.next();
            const double mono = (sample.left + sample.right) * 0.5;
            const double side = (sample.left - sample.right) * 0.5;
            energy += mono * mono;
            sideEnergy += side * side;
            double high = mono;
            for (auto& state : low) {
                state += smoothing * (high - state);
                high -= state;
            }
            highEnergy += high * high;
            const double phase = kTwoPi * 120.0 * i / kSampleRate;
            real += mono * std::cos(phase);
            imaginary += mono * std::sin(phase);
        }
        const double coherentFraction = 2.0 * (real * real + imaginary * imaginary) /
            (kSampleRate * energy);
        require(coherentFraction < 0.35, "motor must not be dominated by one bass sine");
        require(highEnergy / energy > 0.01, "motor needs meaningful mid/high texture");
        require(sideEnergy / energy < 0.01, "HDD should remain a centered point source");
    }
}

void testProfilesHaveDifferentElectricalSignatures() {
    for (const auto profile : {hdd::DriveProfile::WesternDigital, hdd::DriveProfile::Hitachi}) {
        hdd::Synth synth;
        hdd::SynthSettings settings;
        settings.seek = settings.impact = 0.0;
        settings.randomImpacts = false;
        settings.profile = profile;
        synth.setSettings(settings);
        synth.setRunning(true);
        advance(synth, 4.0);
        const double signature = profile == hdd::DriveProfile::WesternDigital ? 2880.0 : 1440.0;
        const double other = profile == hdd::DriveProfile::WesternDigital ? 1440.0 : 2880.0;
        require(frequencyEnergy(synth, signature, other) > 2.0,
                "profiles need acoustically distinct motor signatures");
    }
}

void testSeekAuditionPacket() {
    hdd::Synth synth;
    hdd::SynthSettings settings;
    settings.motor = settings.impact = 0.0;
    settings.randomImpacts = false;
    synth.setSettings(settings);
    synth.setRunning(true);
    advance(synth, 4.0);
    require(synth.seekEventCount() == 0, "settle contacts are not fake disk I/O");
    synth.triggerSeek();
    advance(synth, 0.36);
    require(synth.seekEventCount() >= 8, "manual seek auditions a short chatter packet");
    const auto afterPacket = synth.seekEventCount();
    advance(synth, 0.5);
    require(synth.seekEventCount() == afterPacket, "manual audition finishes by itself");
}

void testProfileChangeDoesNotJump() {
    hdd::Synth switched(kSampleRate, 73), control(kSampleRate, 73);
    hdd::SynthSettings settings;
    settings.seek = settings.impact = 0.0;
    settings.randomImpacts = false;
    switched.setSettings(settings);
    control.setSettings(settings);
    switched.setRunning(true);
    control.setRunning(true);
    advance(switched, 4.0);
    advance(control, 4.0);
    settings.profile = hdd::DriveProfile::Hitachi;
    switched.setSettings(settings);
    const auto changed = switched.next();
    const auto original = control.next();
    require(std::abs(changed.left - original.left) < 0.0001f,
            "profile changes start with a smooth crossfade");
    advance(switched, 0.6);
    require(std::isfinite(switched.next().left), "profile transition remains finite");
}

void testPulseOffPreservesBaseStream() {
    hdd::Synth defaults(kSampleRate, 439), explicitlyOff(kSampleRate, 439), zeroLevel(kSampleRate, 439);
    hdd::SynthSettings offSettings;
    offSettings.pulseNoise = false;
    offSettings.pulse = 1.0;
    explicitlyOff.setSettings(offSettings);
    auto zeroSettings = offSettings;
    zeroSettings.pulseNoise = true;
    zeroSettings.pulse = 0.0;
    zeroLevel.setSettings(zeroSettings);
    // Settle gains while stopped; none of the base RNG streams advances here.
    advance(defaults, 0.5);
    advance(explicitlyOff, 0.5);
    advance(zeroLevel, 0.5);
    for (auto* synth : {&defaults, &explicitlyOff, &zeroLevel}) {
        synth->setRunning(true);
        synth->setActivity(0.72, 120.0);
    }
    for (int i = 0; i < 300000; ++i) {
        if (i == 180000) {
            defaults.triggerImpact();
            explicitlyOff.triggerImpact();
            zeroLevel.triggerImpact();
        }
        const auto expected = defaults.next();
        const auto off = explicitlyOff.next();
        const auto zero = zeroLevel.next();
        require(expected.left == off.left && expected.right == off.right,
                "disabled pulse option must leave the base waveform unchanged");
        require(expected.left == zero.left && expected.right == zero.right,
                "pulse RNG must not alter spindle or seek randomness");
    }
    require(defaults.seekEventCount() == zeroLevel.seekEventCount(),
            "pulse synthesis must not change disk-driven event timing");
}

void testPulseOnlyAndFades() {
    for (const auto profile : {hdd::DriveProfile::WesternDigital, hdd::DriveProfile::Hitachi}) {
        hdd::Synth first(kSampleRate, 817), second(kSampleRate, 817);
        hdd::SynthSettings settings;
        settings.master = 0.6;
        settings.motor = settings.seek = settings.impact = 0.0;
        settings.randomImpacts = false;
        settings.profile = profile;
        settings.pulseNoise = true;
        settings.pulse = 0.75;
        for (auto* synth : {&first, &second}) {
            synth->setSettings(settings);
            synth->setRunning(true);
            advance(*synth, 4.0);
        }
        double energy = 0.0, blockEnergy = 0.0;
        double minBlock = std::numeric_limits<double>::max(), maxBlock = 0.0;
        for (int i = 0; i < 24000; ++i) {
            const auto a = first.next();
            const auto b = second.next();
            require(a.left == b.left && a.right == b.right,
                    "pulse sound must be reproducible from its seed");
            require(std::isfinite(a.left) && std::abs(a.left) < 1.0f,
                    "isolated pulse layer must remain finite and bounded");
            require(a.left == a.right, "pulse sound is a centered mono source");
            const double square = static_cast<double>(a.left) * a.left;
            energy += square;
            blockEnergy += square;
            if (i % 480 == 479) {
                minBlock = std::min(minBlock, blockEnergy);
                maxBlock = std::max(maxBlock, blockEnergy);
                blockEnergy = 0.0;
            }
        }
        require(energy / 24000.0 > 1.0e-5,
                "pulse sound remains audible with all other layer gains at zero");
        require(maxBlock > minBlock * 3.0, "pulse sound has discrete pulse bursts");

        settings.pulseNoise = false;
        first.setSettings(settings);
        double fadeEnergy = 0.0, unchangedEnergy = 0.0;
        for (int i = 0; i < 96; ++i) {
            const auto fading = first.next();
            const auto unchanged = second.next();
            fadeEnergy += static_cast<double>(fading.left) * fading.left;
            unchangedEnergy += static_cast<double>(unchanged.left) * unchanged.left;
        }
        require(fadeEnergy >= unchangedEnergy * 0.70,
                "disabling pulse sound starts a fade rather than cutting abruptly");
        advance(first, 0.6);
        for (int i = 0; i < 1024; ++i) {
            const auto sample = first.next();
            require(sample.left == 0.0f && sample.right == 0.0f,
                    "disabled isolated pulse sound fades to exact silence");
        }

        second.setRunning(false);
        advance(second, 2.5);
        for (int i = 0; i < 1024; ++i) {
            const auto sample = second.next();
            require(sample.left == 0.0f && sample.right == 0.0f,
                    "enabled pulse option remains silent with a stopped spindle");
        }
        settings.pulseNoise = true;
        settings.master = 0.0;
        first.setSettings(settings);
        advance(first, 1.0);
        const auto muted = first.next();
        require(muted.left == 0.0f && muted.right == 0.0f,
                "master gain also mutes the pulse layer");
    }
}

void testPulseGainSanitization() {
    hdd::Synth synth;
    hdd::SynthSettings settings;
    settings.motor = settings.seek = settings.impact = 0.0;
    settings.randomImpacts = false;
    settings.pulseNoise = true;
    settings.pulse = -2.0;
    synth.setSettings(settings);
    synth.setRunning(true);
    advance(synth, 4.0);
    require(synth.next().left == 0.0f, "negative pulse gain clamps to zero");
    settings.pulse = std::numeric_limits<double>::quiet_NaN();
    synth.setSettings(settings);
    advance(synth, 0.2);
    double energy = 0.0;
    for (int i = 0; i < 4800; ++i) {
        const auto sample = synth.next();
        require(std::isfinite(sample.left), "nonfinite pulse gain is contained");
        energy += static_cast<double>(sample.left) * sample.left;
    }
    require(energy > 1.0e-5, "invalid pulse gain falls back to its usable default");
}


void testDiscretePulseTiming() {
    for(double rate:{2.0,8.0,40.0}) {
        hdd::Synth synth;hdd::SynthSettings settings;
        settings.master=0.5;settings.motor=settings.seek=settings.impact=0;
        settings.randomImpacts=false;settings.pulseNoise=true;settings.pulse=0.8;settings.pulseRate=rate;
        synth.setSettings(settings);synth.setRunning(true);advance(synth,4.0);
        const auto before=synth.pulseEventCount();
        double activeEnergy=0,quietEnergy=0;int activeCount=0,quietCount=0,age=100000;
        auto previous=before;
        for(int i=0;i<96000;++i){
            const auto frame=synth.next();const auto count=synth.pulseEventCount();
            if(count!=previous){age=0;previous=count;}
            const double square=static_cast<double>(frame.left)*frame.left;
            if(age<432){activeEnergy+=square;++activeCount;}
            // DC blocker leaves a tiny tail; the quiet interval must be much
            // quieter than the pulse rather than an underlying hiss layer.
            if(age>720){quietEnergy+=square;++quietCount;}
            ++age;
        }
        require(std::abs(static_cast<double>(synth.pulseEventCount()-before)-rate*2)<1.1,
                "pulse count must follow configured repetition rate");
        require(activeCount>0&&quietCount>0&&activeEnergy/activeCount>1e-5,"pulses are audible");
        require((quietEnergy/quietCount)<(activeEnergy/activeCount)*0.0001,
                "pulse periods must have genuinely quiet gaps");
    }
}

double textureRatio(double clarity,hdd::DriveProfile profile=hdd::DriveProfile::WesternDigital) {
    hdd::Synth synth;hdd::SynthSettings settings;
    settings.clarity=clarity;settings.profile=profile;settings.randomImpacts=false;
    settings.motor=1;settings.seek=settings.impact=0;
    synth.setSettings(settings);synth.setRunning(true);advance(synth,4.0);
    double low=0,lowEnergy=0,highEnergy=0;
    const double alpha=1.0-std::exp(-kTwoPi*700.0/kSampleRate);
    for(int i=0;i<48000;++i){const auto f=synth.next();low+=alpha*(f.left-low);
        lowEnergy+=low*low;highEnergy+=(f.left-low)*(f.left-low);}
    return highEnergy/lowEnergy;
}
void testClarityAndNotebookProfiles() {
    require(textureRatio(1)>textureRatio(0)*1.5,"clarity raises presence relative to low body");
    for(const auto profile:{hdd::DriveProfile::Laptop5400,hdd::DriveProfile::Laptop7200}){
        hdd::Synth synth;hdd::SynthSettings settings;
        settings.profile=profile;settings.rpm=profile==hdd::DriveProfile::Laptop5400?5400:7200;
        settings.randomImpacts=false;settings.seek=settings.impact=0;
        synth.setSettings(settings);synth.setRunning(true);advance(synth,4.0);
        require(std::abs(synth.currentRpm()-settings.rpm)<0.01,"notebook requested RPM");
        require(frequencyEnergy(synth,settings.rpm/60*18,settings.rpm/60*24)>3,
                "notebook motor has its own signature, not a desktop RPM alias");
        settings.motor=0;settings.seek=settings.impact=0.6;synth.setSettings(settings);advance(synth,0.6);
        synth.triggerSeek();synth.triggerImpact();advance(synth,0.4);
        require(synth.seekEventCount()>1&&synth.impactEventCount()==1,"notebook transients work");
    }
}
void testMetalContactTransient() {
    for(const auto profile:{hdd::DriveProfile::WesternDigital,hdd::DriveProfile::Hitachi,
            hdd::DriveProfile::Laptop5400,hdd::DriveProfile::Laptop7200}){
        hdd::Synth synth;hdd::SynthSettings settings;
        settings.profile=profile;settings.randomImpacts=false;settings.motor=settings.seek=0;
        synth.setSettings(settings);synth.setRunning(true);advance(synth,4.0);synth.triggerImpact();
        double low=0,lowEnergy=0,highEnergy=0,early=0,late=0;
        const double alpha=1-std::exp(-kTwoPi*800/kSampleRate);
        for(int i=0;i<4800;++i){double x=synth.next().left;low+=alpha*(x-low);
            lowEnergy+=low*low;highEnergy+=(x-low)*(x-low);
            if(i<960)early+=x*x;if(i>2880)late+=x*x;}
        require(early>0.001,"metal contact has an audible attack");
        require(highEnergy>lowEnergy*1.5,"contact is a bright clack, not a bass thud");
        require(late<early*0.001,"metal contact decays without a sustained bell");
    }
}

void testInvalidInputsAreContained() {
    hdd::Synth synth(std::numeric_limits<double>::quiet_NaN(), 0);
    hdd::SynthSettings settings;
    settings.rpm = std::numeric_limits<double>::infinity();
    settings.master = std::numeric_limits<double>::quiet_NaN();
    settings.impactInterval = -1.0;
    synth.setSettings(settings);
    synth.setActivity(std::numeric_limits<double>::quiet_NaN(), -400.0);
    synth.setRunning(true);
    advance(synth, 3.2);
    require(std::isfinite(synth.currentRpm()), "invalid settings contained");
    require(synth.currentRpm() > 0.0, "invalid settings fall back to usable defaults");
}
} // namespace

int main() {
    try {
        testSpindleAndSilence();
        testReversal();
        testDeterministicSeed();
        testActivityResponseAndManualEvents();
        testBoundedExtremeMix();
        testRpmControlsElectricalOrders();
        testMotorIsBroadbandAndCentered();
        testProfilesHaveDifferentElectricalSignatures();
        testSeekAuditionPacket();
        testProfileChangeDoesNotJump();
        testPulseOffPreservesBaseStream();
        testPulseOnlyAndFades();
        testPulseGainSanitization();
        testDiscretePulseTiming();
        testClarityAndNotebookProfiles();
        testMetalContactTransient();
        testInvalidInputsAreContained();
        std::cout << "All procedural HDD synth tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Synth test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
