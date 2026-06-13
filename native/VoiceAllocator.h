#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

class VoiceAllocator
{
public:
    static constexpr int maxVoices = 16;
    static constexpr int controlChannelsPerVoice = 4;
    static constexpr int numControlChannels = maxVoices * controlChannelsPerVoice;

    enum ControlChannelOffset
    {
        triggerOffset = 0,
        gateOffset = maxVoices,
        pitchOffset = maxVoices * 2,
        velocityOffset = maxVoices * 3
    };

    void prepare(int maximumBlockSize);
    void reset();
    void render(
        const juce::MidiBuffer& midi,
        int numSamples,
        float rootNote,
        float transposeSemitones,
        float releaseSeconds,
        double sampleRate);

    const float** getChannelPointers() noexcept { return channelPointers.data(); }
    int getActiveVoiceCount() const noexcept;
    int getMaximumBlockSize() const noexcept { return controls.getNumSamples(); }

private:
    struct Voice
    {
        int note = -1;
        float velocity = 0.0f;
        bool active = false;
        bool releasing = false;
        int64_t startedAt = 0;
        int64_t releasedAt = 0;
        int64_t releaseSamplesRemaining = 0;
    };

    int allocateVoice();
    void noteOn(int note, float velocity, int sampleOffset);
    void noteOff(int note);
    void fillControls(int startSample, int numSamples, float rootNote, float transposeSemitones);
    void advanceReleaseCounters(int numSamples);

    std::array<Voice, maxVoices> voices;
    juce::AudioBuffer<float> controls;
    std::array<const float*, numControlChannels> channelPointers {};
    int64_t eventCounter = 0;
    int64_t releaseLengthSamples = 0;
};
