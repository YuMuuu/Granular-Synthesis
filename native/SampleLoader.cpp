#include "SampleLoader.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr double maxSampleDurationSeconds = 60.0;
constexpr size_t waveformPeakCount = 512;

std::vector<float> buildWaveformPeaks(const juce::AudioBuffer<float>& buffer)
{
    std::vector<float> peaks(waveformPeakCount, 0.0f);
    const auto numSamples = buffer.getNumSamples();

    if (numSamples == 0)
        return peaks;

    const auto* samples = buffer.getReadPointer(0);

    for (size_t peakIndex = 0; peakIndex < peaks.size(); ++peakIndex)
    {
        const auto start = static_cast<int>((peakIndex * static_cast<size_t>(numSamples)) / peaks.size());
        const auto end = static_cast<int>(((peakIndex + 1) * static_cast<size_t>(numSamples)) / peaks.size());
        float peak = 0.0f;

        for (int sampleIndex = start; sampleIndex < std::max(start + 1, end); ++sampleIndex)
            peak = std::max(peak, std::abs(samples[juce::jmin(sampleIndex, numSamples - 1)]));

        peaks[peakIndex] = peak;
    }

    return peaks;
}
}

SampleLoader::SampleLoader(Completion completionIn)
    : Thread("Granular Sample Loader"),
      completion(std::move(completionIn))
{
    startThread();
}

SampleLoader::~SampleLoader()
{
    shutdown();
}

void SampleLoader::shutdown()
{
    if (!isThreadRunning())
        return;

    signalThreadShouldExit();
    notify();
    stopThread(5000);
}

void SampleLoader::importFile(const juce::File& sourceFile)
{
    {
        const juce::ScopedLock lock(requestLock);
        pendingRequest = Request { sourceFile, {}, true };
    }

    notify();
}

void SampleLoader::restoreSample(const juce::String& sampleId)
{
    {
        const juce::ScopedLock lock(requestLock);
        pendingRequest = Request { getManagedSampleFile(sampleId), sampleId, false };
    }

    notify();
}

juce::File SampleLoader::getSampleStorageDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Granular Synthesis")
        .getChildFile("Samples");
}

juce::File SampleLoader::getManagedSampleFile(const juce::String& sampleId)
{
    return getSampleStorageDirectory().getChildFile(sampleId + ".wav");
}

void SampleLoader::run()
{
    while (!threadShouldExit())
    {
        wait(-1);

        if (threadShouldExit())
            break;

        std::optional<Request> request;

        {
            const juce::ScopedLock lock(requestLock);
            request = std::exchange(pendingRequest, std::nullopt);
        }

        if (request.has_value())
            completion(load(*request));
    }
}

SampleLoadResult SampleLoader::load(const Request& request) const
{
    SampleLoadResult result;

    if (!request.file.existsAsFile())
    {
        result.error = request.shouldImport
            ? "The selected WAV file does not exist."
            : "The managed WAV file is missing. Re-link the sample.";
        result.metadata.sampleId = request.sampleId;
        return result;
    }

    if (!request.file.hasFileExtension("wav"))
    {
        result.error = "Only WAV files are supported.";
        return result;
    }

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> reader(
        wavFormat.createReaderFor(request.file.createInputStream().release(), true));

    if (reader == nullptr)
    {
        result.error = "The WAV file could not be decoded.";
        return result;
    }

    if (reader->numChannels < 1 || reader->numChannels > 2)
    {
        result.error = "The WAV file must be mono or stereo.";
        return result;
    }

    const auto durationSeconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;

    if (durationSeconds > maxSampleDurationSeconds)
    {
        result.error = "The WAV file must be 60 seconds or shorter.";
        return result;
    }

    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        result.error = "The WAV file has an unsupported length.";
        return result;
    }

    const auto numFrames = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> sourceBuffer(static_cast<int>(reader->numChannels), numFrames);

    if (!reader->read(&sourceBuffer, 0, numFrames, 0, true, true))
    {
        result.error = "The WAV file could not be read.";
        return result;
    }

    result.monoBuffer.setSize(1, numFrames);
    result.monoBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numFrames);

    if (reader->numChannels == 2)
    {
        result.monoBuffer.addFrom(0, 0, sourceBuffer, 1, 0, numFrames);
        result.monoBuffer.applyGain(0.5f);
    }

    auto sampleId = request.sampleId;
    const auto fileHash = juce::SHA256(request.file).toHexString();

    if (request.shouldImport)
    {
        sampleId = fileHash;
        const auto storageDirectory = getSampleStorageDirectory();

        if (!storageDirectory.createDirectory())
        {
            result.error = "The sample storage directory could not be created.";
            return result;
        }

        const auto managedFile = getManagedSampleFile(sampleId);

        if (!managedFile.existsAsFile() && !request.file.copyFileTo(managedFile))
        {
            result.error = "The WAV file could not be copied to sample storage.";
            return result;
        }
    }

    result.metadata = {
        sampleId,
        getManagedSampleFile(sampleId).getFileName(),
        request.shouldImport ? request.file.getFileName() : juce::String(),
        fileHash,
        reader->sampleRate,
        reader->lengthInSamples,
        durationSeconds
    };
    result.waveformPeaks = buildWaveformPeaks(result.monoBuffer);
    return result;
}
