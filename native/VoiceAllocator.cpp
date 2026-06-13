#include "VoiceAllocator.h"

#include <algorithm>
#include <cmath>
#include <limits>

void VoiceAllocator::prepare(int maximumBlockSize)
{
    controls.setSize(numControlChannels, maximumBlockSize, false, true, false);

    for (int channel = 0; channel < numControlChannels; ++channel)
        channelPointers[static_cast<size_t>(channel)] = controls.getReadPointer(channel);

    reset();
}

void VoiceAllocator::reset()
{
    voices = {};
    controls.clear();
    eventCounter = 0;
    releaseLengthSamples = 0;
}

void VoiceAllocator::render(
    const juce::MidiBuffer& midi,
    int numSamples,
    float rootNote,
    float transposeSemitones,
    float releaseSeconds,
    double sampleRate)
{
    jassert(numSamples <= controls.getNumSamples());
    controls.clear();
    releaseLengthSamples = std::max<int64_t>(
        1,
        static_cast<int64_t>(std::ceil(std::max(0.0f, releaseSeconds) * sampleRate)));

    int segmentStart = 0;

    for (const auto metadata : midi)
    {
        const auto sampleOffset = juce::jlimit(0, numSamples, metadata.samplePosition);
        fillControls(segmentStart, sampleOffset - segmentStart, rootNote, transposeSemitones);
        advanceReleaseCounters(sampleOffset - segmentStart);

        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            noteOn(message.getNoteNumber(), message.getFloatVelocity(), sampleOffset);
        else if (message.isNoteOff())
            noteOff(message.getNoteNumber());
        else if (message.isAllNotesOff() || message.isAllSoundOff())
            for (auto& voice : voices)
                if (voice.active && !voice.releasing)
                {
                    voice.releasing = true;
                    voice.releasedAt = ++eventCounter;
                    voice.releaseSamplesRemaining = releaseLengthSamples;
                }

        segmentStart = sampleOffset;
    }

    fillControls(segmentStart, numSamples - segmentStart, rootNote, transposeSemitones);
    advanceReleaseCounters(numSamples - segmentStart);
}

int VoiceAllocator::getActiveVoiceCount() const noexcept
{
    return static_cast<int>(std::count_if(voices.begin(), voices.end(), [](const Voice& voice)
    {
        return voice.active;
    }));
}

int VoiceAllocator::allocateVoice()
{
    for (int index = 0; index < maxVoices; ++index)
        if (!voices[static_cast<size_t>(index)].active)
            return index;

    auto oldestRelease = std::numeric_limits<int64_t>::max();
    int releaseVoice = -1;

    for (int index = 0; index < maxVoices; ++index)
    {
        const auto& voice = voices[static_cast<size_t>(index)];

        if (voice.releasing && voice.releasedAt < oldestRelease)
        {
            oldestRelease = voice.releasedAt;
            releaseVoice = index;
        }
    }

    if (releaseVoice >= 0)
        return releaseVoice;

    auto oldestStart = std::numeric_limits<int64_t>::max();
    int activeVoice = 0;

    for (int index = 0; index < maxVoices; ++index)
    {
        const auto& voice = voices[static_cast<size_t>(index)];

        if (voice.startedAt < oldestStart)
        {
            oldestStart = voice.startedAt;
            activeVoice = index;
        }
    }

    return activeVoice;
}

void VoiceAllocator::noteOn(int note, float velocity, int sampleOffset)
{
    const auto voiceIndex = allocateVoice();
    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    voice.note = note;
    voice.velocity = velocity;
    voice.active = true;
    voice.releasing = false;
    voice.startedAt = ++eventCounter;
    voice.releasedAt = 0;
    voice.releaseSamplesRemaining = 0;

    if (sampleOffset < controls.getNumSamples())
        controls.setSample(triggerOffset + voiceIndex, sampleOffset, 1.0f);
}

void VoiceAllocator::noteOff(int note)
{
    Voice* oldestMatchingVoice = nullptr;

    for (auto& voice : voices)
    {
        if (voice.active
            && !voice.releasing
            && voice.note == note
            && (oldestMatchingVoice == nullptr || voice.startedAt < oldestMatchingVoice->startedAt))
        {
            oldestMatchingVoice = &voice;
        }
    }

    if (oldestMatchingVoice == nullptr)
        return;

    oldestMatchingVoice->releasing = true;
    oldestMatchingVoice->releasedAt = ++eventCounter;
    oldestMatchingVoice->releaseSamplesRemaining = releaseLengthSamples;
}

void VoiceAllocator::fillControls(
    int startSample,
    int numSamples,
    float rootNote,
    float transposeSemitones)
{
    if (numSamples <= 0)
        return;

    for (int voiceIndex = 0; voiceIndex < maxVoices; ++voiceIndex)
    {
        const auto& voice = voices[static_cast<size_t>(voiceIndex)];
        const auto gate = voice.active && !voice.releasing ? 1.0f : 0.0f;
        const auto pitchRatio = voice.active
            ? std::exp2((static_cast<float>(voice.note) - rootNote + transposeSemitones) / 12.0f)
            : 1.0f;
        const auto velocity = voice.active ? voice.velocity : 0.0f;

        juce::FloatVectorOperations::fill(
            controls.getWritePointer(gateOffset + voiceIndex, startSample),
            gate,
            numSamples);
        juce::FloatVectorOperations::fill(
            controls.getWritePointer(pitchOffset + voiceIndex, startSample),
            pitchRatio,
            numSamples);
        juce::FloatVectorOperations::fill(
            controls.getWritePointer(velocityOffset + voiceIndex, startSample),
            velocity,
            numSamples);
    }
}

void VoiceAllocator::advanceReleaseCounters(int numSamples)
{
    if (numSamples <= 0)
        return;

    for (auto& voice : voices)
    {
        if (!voice.active || !voice.releasing)
            continue;

        voice.releaseSamplesRemaining -= numSamples;

        if (voice.releaseSamplesRemaining <= 0)
            voice = {};
    }
}
