#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>

class PhaseReconstructor
{
public:
    static constexpr int fftSize = 2048;
    static constexpr int hopSize = 512;
    static constexpr int iterationCount = 8;
    static constexpr int latencySamples = fftSize;

    PhaseReconstructor();
    ~PhaseReconstructor();

    void prepare(double sampleRate);
    void reset();

    // Returns false if a non-finite value was detected and replaced with silence.
    bool process(juce::AudioBuffer<float>& buffer, bool enabled) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
