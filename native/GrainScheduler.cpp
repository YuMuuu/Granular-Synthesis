#include "GrainScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr float pi = 3.14159265358979323846f;

float approximateWindowAmplitude(double phase)
{
    return 0.5f - 0.5f * std::cos(2.0f * pi * static_cast<float>(phase));
}

double wrapToRegion(double position, double regionStart, double regionEnd)
{
    const auto width = regionEnd - regionStart;

    if (width <= 0.0)
        return regionStart;

    auto wrapped = std::fmod(position - regionStart, width);

    if (wrapped < 0.0)
        wrapped += width;

    return regionStart + wrapped;
}
}

void GrainScheduler::prepare(int maximumBlockSize, double sampleRate)
{
    controls.setSize(numControlChannels, maximumBlockSize, false, true, false);

    for (int channel = 0; channel < numControlChannels; ++channel)
        channelPointers[static_cast<size_t>(channel)] = controls.getReadPointer(channel);

    hostSampleRate = sampleRate;
    reset();
}

void GrainScheduler::reset()
{
    grains = {};
    pendingGrains = {};
    voices = {};
    controls.clear();
    ageCounter = 0;
    randomState = randomSeed;
}

void GrainScheduler::setRandomSeed(uint32_t seed) noexcept
{
    randomSeed = seed != 0 ? seed : 0x47525359u;
    randomState = randomSeed;
}

void GrainScheduler::render(
    const float* const* voiceControls,
    int numVoiceControlChannels,
    int numSamples,
    int voiceCount,
    int voiceTriggerOffset,
    int voiceGateOffset,
    int voicePitchOffset,
    int voiceVelocityOffset,
    const Parameters& parameters,
    double sourceSampleRate,
    int64_t sourceFrames)
{
    jassert(numSamples <= controls.getNumSamples());
    controls.clear();

    if (voiceControls == nullptr
        || numVoiceControlChannels < voiceVelocityOffset + voiceCount
        || sourceSampleRate <= 0.0
        || sourceFrames <= 1)
        return;

    const auto density = std::max(1.0f, parameters.densityHz);
    const auto grainIntervalSamples = hostSampleRate / density;
    const auto regionStart = juce::jlimit(0.0, 1.0, static_cast<double>(parameters.regionStart));
    const auto regionEnd = juce::jlimit(regionStart, 1.0, static_cast<double>(parameters.regionEnd));

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        for (int voice = 0; voice < voiceCount; ++voice)
        {
            const auto trigger = voiceControls[voiceTriggerOffset + voice][sampleIndex] > 0.5f;
            const auto gateHigh = voiceControls[voiceGateOffset + voice][sampleIndex] > 0.5f;
            const auto pitchRatio = voiceControls[voicePitchOffset + voice][sampleIndex];
            const auto velocity = voiceControls[voiceVelocityOffset + voice][sampleIndex];
            auto& voiceState = voices[static_cast<size_t>(voice)];

            if (trigger || (gateHigh && !voiceState.wasGateHigh))
            {
                fadeGrainsForVoice(voice);
                voiceState.samplesUntilNextGrain = 0.0;
                voiceState.scanPosition = juce::jlimit(
                    regionStart,
                    regionEnd,
                    static_cast<double>(parameters.position));
            }

            updateVoiceEnvelope(voiceState, gateHigh, trigger, parameters);

            if (gateHigh)
            {
                if (parameters.freeze)
                {
                    voiceState.scanPosition = juce::jlimit(
                        regionStart,
                        regionEnd,
                        static_cast<double>(parameters.position));
                }
                else
                {
                    voiceState.scanPosition = wrapToRegion(
                        voiceState.scanPosition
                            + static_cast<double>(parameters.scanRate) / hostSampleRate,
                        regionStart,
                        regionEnd);
                }

                if (voiceState.samplesUntilNextGrain <= 0.0)
                {
                    scheduleGrain(makeGrain(
                        voice,
                        pitchRatio,
                        velocity,
                        parameters,
                        sourceSampleRate,
                        sourceFrames));
                    voiceState.samplesUntilNextGrain += grainIntervalSamples;
                }

                voiceState.samplesUntilNextGrain -= 1.0;
            }

            voiceState.wasGateHigh = gateHigh;
        }

        for (int lane = 0; lane < maxGrains; ++lane)
            writeGrainSample(lane, sampleIndex, parameters);
    }
}

int GrainScheduler::getActiveGrainCount() const noexcept
{
    return static_cast<int>(std::count_if(grains.begin(), grains.end(), [](const GrainState& grain)
    {
        return grain.active;
    }));
}

uint32_t GrainScheduler::nextRandom()
{
    auto x = randomState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    randomState = x;
    return x;
}

float GrainScheduler::randomUnit()
{
    return static_cast<float>(nextRandom()) / static_cast<float>(std::numeric_limits<uint32_t>::max());
}

float GrainScheduler::randomSigned()
{
    return randomUnit() * 2.0f - 1.0f;
}

GrainScheduler::GrainState GrainScheduler::makeGrain(
    int voice,
    float pitchRatio,
    float velocity,
    const Parameters& parameters,
    double sourceSampleRate,
    int64_t sourceFrames)
{
    const auto regionStart = juce::jlimit(0.0, 1.0, static_cast<double>(parameters.regionStart));
    const auto regionEnd = juce::jlimit(regionStart, 1.0, static_cast<double>(parameters.regionEnd));
    const auto regionWidth = regionEnd - regionStart;
    const auto sizeScale = juce::jlimit(
        0.05f,
        2.0f,
        1.0f + randomSigned() * parameters.sizeScatter);
    const auto grainSizeSeconds = juce::jlimit(
        0.005,
        0.5,
        static_cast<double>(parameters.grainSizeMs * sizeScale) / 1000.0);
    const auto positionOffset = randomSigned()
        * static_cast<double>(parameters.positionScatter)
        * regionWidth
        * 0.5;
    const auto pitchScatterRatio = std::exp2(
        static_cast<double>(randomSigned() * parameters.pitchScatterSemitones) / 12.0);
    const auto reverse = randomUnit() < parameters.reverseProbability;
    const auto direction = reverse ? -1.0 : 1.0;
    const auto basePosition = voices[static_cast<size_t>(voice)].scanPosition;

    GrainState grain;
    grain.active = true;
    grain.voice = voice;
    grain.phase = 0.0;
    grain.phaseIncrement = 1.0 / (grainSizeSeconds * hostSampleRate);
    grain.readPosition = wrapToRegion(basePosition + positionOffset, regionStart, regionEnd);
    grain.readIncrement = direction
        * static_cast<double>(pitchRatio)
        * pitchScatterRatio
        * sourceSampleRate
        / hostSampleRate
        / static_cast<double>(sourceFrames - 1);
    grain.pan = randomSigned() * parameters.stereoWidth;
    grain.velocity = velocity;
    grain.age = ++ageCounter;
    return grain;
}

int GrainScheduler::chooseGrainLane() const
{
    for (int lane = 0; lane < maxGrains; ++lane)
        if (!grains[static_cast<size_t>(lane)].active
            && !pendingGrains[static_cast<size_t>(lane)].pending)
            return lane;

    auto bestAmplitude = std::numeric_limits<float>::max();
    auto oldestAge = std::numeric_limits<int64_t>::max();
    int bestLane = 0;

    for (int lane = 0; lane < maxGrains; ++lane)
    {
        if (pendingGrains[static_cast<size_t>(lane)].pending)
            continue;

        const auto& grain = grains[static_cast<size_t>(lane)];
        const auto amplitude = approximateWindowAmplitude(grain.phase) * grain.velocity;

        if (amplitude < bestAmplitude
            || (std::abs(amplitude - bestAmplitude) <= std::numeric_limits<float>::epsilon()
                && grain.age < oldestAge))
        {
            bestAmplitude = amplitude;
            oldestAge = grain.age;
            bestLane = lane;
        }
    }

    return bestAmplitude < std::numeric_limits<float>::max() ? bestLane : -1;
}

void GrainScheduler::scheduleGrain(const GrainState& grain)
{
    const auto lane = chooseGrainLane();

    if (lane < 0)
        return;

    auto& active = grains[static_cast<size_t>(lane)];
    auto& pending = pendingGrains[static_cast<size_t>(lane)];

    if (!active.active)
    {
        active = grain;
        pending = {};
        return;
    }

    pending.pending = true;
    pending.grain = grain;
    pending.fadeSamplesTotal = std::max(1, static_cast<int>(std::ceil(0.005 * hostSampleRate)));
    pending.fadeSamplesRemaining = pending.fadeSamplesTotal;
}

void GrainScheduler::writeGrainSample(int lane, int sampleIndex, const Parameters& parameters)
{
    auto& grain = grains[static_cast<size_t>(lane)];
    auto& pending = pendingGrains[static_cast<size_t>(lane)];

    if (!grain.active)
        return;

    const auto regionStart = juce::jlimit(0.0, 1.0, static_cast<double>(parameters.regionStart));
    const auto regionEnd = juce::jlimit(regionStart, 1.0, static_cast<double>(parameters.regionEnd));
    auto fadeGain = 1.0f;

    if (pending.pending)
    {
        fadeGain = static_cast<float>(pending.fadeSamplesRemaining)
            / static_cast<float>(pending.fadeSamplesTotal);
        --pending.fadeSamplesRemaining;
    }

    if (grain.forcedFadeSamplesRemaining > 0)
    {
        fadeGain *= static_cast<float>(grain.forcedFadeSamplesRemaining)
            / static_cast<float>(grain.forcedFadeSamplesTotal);
        --grain.forcedFadeSamplesRemaining;
    }

    const auto leftGain = grain.velocity * fadeGain * (1.0f - grain.pan) * 0.5f;
    const auto envelope = grain.voice >= 0
        ? voices[static_cast<size_t>(grain.voice)].envelope
        : 0.0f;
    const auto leftGainWithEnvelope = leftGain * envelope;
    const auto rightGain = grain.velocity * fadeGain * envelope * (1.0f + grain.pan) * 0.5f;
    controls.setSample(positionOffset + lane, sampleIndex, static_cast<float>(grain.readPosition));
    controls.setSample(phaseOffset + lane, sampleIndex, static_cast<float>(grain.phase));
    controls.setSample(leftGainOffset + lane, sampleIndex, leftGainWithEnvelope);
    controls.setSample(rightGainOffset + lane, sampleIndex, rightGain);

    grain.phase += grain.phaseIncrement;
    grain.readPosition = wrapToRegion(grain.readPosition + grain.readIncrement, regionStart, regionEnd);

    if (grain.forcedFadeSamplesTotal > 0 && grain.forcedFadeSamplesRemaining <= 0)
    {
        grain = {};
        pending = {};
    }
    else if (pending.pending && pending.fadeSamplesRemaining <= 0)
    {
        grain = pending.grain;
        pending = {};
    }
    else if (grain.phase >= 1.0)
    {
        grain = {};
        pending = {};
    }
}

void GrainScheduler::fadeGrainsForVoice(int voice)
{
    const auto fadeSamples = std::max(1, static_cast<int>(std::ceil(0.005 * hostSampleRate)));

    for (auto& grain : grains)
    {
        if (!grain.active || grain.voice != voice)
            continue;

        grain.forcedFadeSamplesTotal = fadeSamples;
        grain.forcedFadeSamplesRemaining = fadeSamples;
    }
}

void GrainScheduler::updateVoiceEnvelope(
    VoiceState& voice,
    bool gateHigh,
    bool trigger,
    const Parameters& parameters)
{
    if (trigger || (gateHigh && !voice.wasGateHigh))
        voice.envelopeStage = VoiceState::EnvelopeStage::attack;
    else if (!gateHigh && voice.wasGateHigh)
        voice.envelopeStage = VoiceState::EnvelopeStage::release;

    const auto attackSamples = std::max(1.0, parameters.attackSeconds * hostSampleRate);
    const auto decaySamples = std::max(1.0, parameters.decaySeconds * hostSampleRate);
    const auto releaseSamples = std::max(1.0, parameters.releaseSeconds * hostSampleRate);
    const auto sustain = juce::jlimit(0.0f, 1.0f, parameters.sustain);

    switch (voice.envelopeStage)
    {
        case VoiceState::EnvelopeStage::idle:
            voice.envelope = 0.0f;
            break;
        case VoiceState::EnvelopeStage::attack:
            voice.envelope += static_cast<float>(1.0 / attackSamples);

            if (voice.envelope >= 1.0f)
            {
                voice.envelope = 1.0f;
                voice.envelopeStage = VoiceState::EnvelopeStage::decay;
            }
            break;
        case VoiceState::EnvelopeStage::decay:
            voice.envelope -= static_cast<float>((1.0 - sustain) / decaySamples);

            if (voice.envelope <= sustain)
            {
                voice.envelope = sustain;
                voice.envelopeStage = VoiceState::EnvelopeStage::sustain;
            }
            break;
        case VoiceState::EnvelopeStage::sustain:
            voice.envelope = sustain;
            break;
        case VoiceState::EnvelopeStage::release:
            voice.envelope -= static_cast<float>(1.0 / releaseSamples);

            if (voice.envelope <= 0.0f)
            {
                voice.envelope = 0.0f;
                voice.envelopeStage = VoiceState::EnvelopeStage::idle;
            }
            break;
    }
}
