#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

class GrainScheduler
{
public:
    static constexpr int maxGrains = 128;
    static constexpr int channelsPerGrain = 4;
    static constexpr int numControlChannels = maxGrains * channelsPerGrain;

    enum ControlChannelOffset
    {
        positionOffset = 0,
        phaseOffset = maxGrains,
        leftGainOffset = maxGrains * 2,
        rightGainOffset = maxGrains * 3
    };

    struct Parameters
    {
        float regionStart = 0.0f;
        float regionEnd = 1.0f;
        float position = 0.5f;
        float scanRate = 0.0f;
        bool freeze = false;
        float grainSizeMs = 100.0f;
        float densityHz = 20.0f;
        float positionScatter = 0.0f;
        float sizeScatter = 0.0f;
        float pitchScatterSemitones = 0.0f;
        float reverseProbability = 0.0f;
        float stereoWidth = 0.5f;
        float attackSeconds = 0.01f;
        float decaySeconds = 0.1f;
        float sustain = 0.8f;
        float releaseSeconds = 0.5f;
    };

    void prepare(int maximumBlockSize, double sampleRate);
    void reset();
    void render(
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
        int64_t sourceFrames);

    const float** getChannelPointers() noexcept { return channelPointers.data(); }
    int getActiveGrainCount() const noexcept;
    int getMaximumBlockSize() const noexcept { return controls.getNumSamples(); }
    uint32_t getRandomSeed() const noexcept { return randomSeed; }
    void setRandomSeed(uint32_t seed) noexcept;

private:
    struct GrainState
    {
        bool active = false;
        int voice = -1;
        double phase = 0.0;
        double phaseIncrement = 0.0;
        double readPosition = 0.0;
        double readIncrement = 0.0;
        float pan = 0.0f;
        float velocity = 0.0f;
        int64_t age = 0;
        int forcedFadeSamplesRemaining = 0;
        int forcedFadeSamplesTotal = 0;
    };

    struct PendingGrain
    {
        bool pending = false;
        GrainState grain;
        int fadeSamplesRemaining = 0;
        int fadeSamplesTotal = 0;
    };

    struct VoiceState
    {
        enum class EnvelopeStage
        {
            idle,
            attack,
            decay,
            sustain,
            release
        };

        double samplesUntilNextGrain = 0.0;
        double scanPosition = 0.5;
        float envelope = 0.0f;
        EnvelopeStage envelopeStage = EnvelopeStage::idle;
        bool wasGateHigh = false;
    };

    uint32_t nextRandom();
    float randomUnit();
    float randomSigned();
    GrainState makeGrain(
        int voice,
        float pitchRatio,
        float velocity,
        const Parameters& parameters,
        double sourceSampleRate,
        int64_t sourceFrames);
    int chooseGrainLane() const;
    void scheduleGrain(const GrainState& grain);
    void writeGrainSample(int lane, int sampleIndex, const Parameters& parameters);
    void updateVoiceEnvelope(VoiceState& voice, bool gateHigh, bool trigger, const Parameters& parameters);
    void fadeGrainsForVoice(int voice);

    std::array<GrainState, maxGrains> grains;
    std::array<PendingGrain, maxGrains> pendingGrains;
    std::array<VoiceState, 16> voices;
    juce::AudioBuffer<float> controls;
    std::array<const float*, numControlChannels> channelPointers {};
    double hostSampleRate = 0.0;
    int64_t ageCounter = 0;
    uint32_t randomSeed = 0x47525359u;
    uint32_t randomState = randomSeed;
};
