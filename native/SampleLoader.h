#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <vector>

struct SampleMetadata
{
    juce::String sampleId;
    juce::String fileName;
    juce::String originalFileName;
    juce::String fileHash;
    double sampleRate = 0.0;
    int64_t numFrames = 0;
    double durationSeconds = 0.0;
};

struct SampleLoadResult
{
    SampleMetadata metadata;
    juce::AudioBuffer<float> monoBuffer;
    std::vector<float> waveformPeaks;
    juce::String error;

    bool succeeded() const noexcept { return error.isEmpty(); }
};

class SampleLoader final : private juce::Thread
{
public:
    using Completion = std::function<void(SampleLoadResult)>;

    explicit SampleLoader(Completion completion);
    ~SampleLoader() override;

    void shutdown();
    void importFile(const juce::File& sourceFile);
    void restoreSample(const juce::String& sampleId);

    static juce::File getSampleStorageDirectory();
    static juce::File getManagedSampleFile(const juce::String& sampleId);

private:
    struct Request
    {
        juce::File file;
        juce::String sampleId;
        bool shouldImport = false;
    };

    void run() override;
    SampleLoadResult load(const Request& request) const;

    Completion completion;
    juce::CriticalSection requestLock;
    std::optional<Request> pendingRequest;
};
