#include "PhaseReconstructor.h"

#define POCKETFFT_NO_MULTITHREADING
#include "third_party/pocketfft/pocketfft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace
{
constexpr int numChannels = 2;
constexpr int numBins = PhaseReconstructor::fftSize / 2 + 1;
constexpr float pi = 3.14159265358979323846f;

using SampleBlock = std::array<float, PhaseReconstructor::fftSize>;
using Spectrum = std::array<std::complex<float>, numBins>;

struct ChannelState
{
    SampleBlock inputRing {};
    SampleBlock delayRing {};
    SampleBlock overlapAddRing {};
    SampleBlock transform {};
    SampleBlock reanalysis {};
    SampleBlock scratch {};
    Spectrum spectrum {};
    std::array<float, numBins> targetMagnitude {};
    int inputWriteIndex = 0;
    int delayWriteIndex = 0;
    int overlapReadIndex = 0;
};

int wrapIndex(int index) noexcept
{
    return index & (PhaseReconstructor::fftSize - 1);
}

void unpackSpectrum(const SampleBlock& packed, Spectrum& spectrum) noexcept
{
    spectrum[0] = { packed[0], 0.0f };

    for (int bin = 1; bin < numBins - 1; ++bin)
        spectrum[static_cast<size_t>(bin)] = {
            packed[static_cast<size_t>(2 * bin - 1)],
            packed[static_cast<size_t>(2 * bin)]
        };

    spectrum[numBins - 1] = { packed[PhaseReconstructor::fftSize - 1], 0.0f };
}

void packSpectrum(const Spectrum& spectrum, SampleBlock& packed) noexcept
{
    packed[0] = spectrum[0].real();

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        packed[static_cast<size_t>(2 * bin - 1)] = spectrum[static_cast<size_t>(bin)].real();
        packed[static_cast<size_t>(2 * bin)] = spectrum[static_cast<size_t>(bin)].imag();
    }

    packed[PhaseReconstructor::fftSize - 1] = spectrum[numBins - 1].real();
}
}

struct PhaseReconstructor::Impl
{
    void prepare(double newSampleRate)
    {
        sampleRate = std::max(1.0, newSampleRate);
        phaseMixStep = static_cast<float>(1.0 / (sampleRate * 0.005));
        fftPlan = std::make_unique<pocketfft::detail::pocketfft_r<float>>(fftSize);

        for (int sample = 0; sample < fftSize; ++sample)
            window[static_cast<size_t>(sample)] =
                0.5f - 0.5f * std::cos(2.0f * pi * static_cast<float>(sample) / fftSize);

        for (int sample = 0; sample < fftSize; ++sample)
        {
            float squaredWindowSum = 0.0f;

            for (int overlap = 0; overlap < fftSize / hopSize; ++overlap)
            {
                const auto index = wrapIndex(sample + overlap * hopSize);
                const auto value = window[static_cast<size_t>(index)];
                squaredWindowSum += value * value;
            }

            synthesisWindow[static_cast<size_t>(sample)] =
                window[static_cast<size_t>(sample)] / std::max(squaredWindowSum, 1.0e-12f);
        }

        reset();
    }

    void reset() noexcept
    {
        channels = {};
        samplesUntilFrame = fftSize;
        phaseMix = 0.0f;
    }

    bool process(juce::AudioBuffer<float>& buffer, bool enabled) noexcept
    {
        if (fftPlan == nullptr || buffer.getNumChannels() < numChannels)
        {
            buffer.clear();
            return false;
        }

        auto finite = true;
        const auto numSamples = buffer.getNumSamples();
        const auto targetMix = enabled ? 1.0f : 0.0f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto& state = channels[static_cast<size_t>(channel)];
                auto* output = buffer.getWritePointer(channel);
                const auto input = std::isfinite(output[sample]) ? output[sample] : 0.0f;

                if (!std::isfinite(output[sample]))
                    finite = false;

                const auto delayed = state.delayRing[static_cast<size_t>(state.delayWriteIndex)];
                state.delayRing[static_cast<size_t>(state.delayWriteIndex)] = input;
                state.delayWriteIndex = wrapIndex(state.delayWriteIndex + 1);

                state.inputRing[static_cast<size_t>(state.inputWriteIndex)] = input;
                state.inputWriteIndex = wrapIndex(state.inputWriteIndex + 1);

                const auto reconstructed =
                    state.overlapAddRing[static_cast<size_t>(state.overlapReadIndex)];
                state.overlapAddRing[static_cast<size_t>(state.overlapReadIndex)] = 0.0f;
                state.overlapReadIndex = wrapIndex(state.overlapReadIndex + 1);

                const auto mixed = delayed + phaseMix * (reconstructed - delayed);
                output[sample] = std::isfinite(mixed) ? mixed : 0.0f;

                if (!std::isfinite(mixed))
                    finite = false;
            }

            if (phaseMix < targetMix)
                phaseMix = std::min(targetMix, phaseMix + phaseMixStep);
            else if (phaseMix > targetMix)
                phaseMix = std::max(targetMix, phaseMix - phaseMixStep);

            if (--samplesUntilFrame == 0)
            {
                if (enabled)
                {
                    for (auto& channel : channels)
                        finite = reconstructFrame(channel) && finite;
                }

                samplesUntilFrame = hopSize;
            }
        }

        return finite;
    }

    bool reconstructFrame(ChannelState& state) noexcept
    {
        for (int sample = 0; sample < fftSize; ++sample)
        {
            const auto inputIndex = wrapIndex(state.inputWriteIndex + sample);
            state.transform[static_cast<size_t>(sample)] =
                state.inputRing[static_cast<size_t>(inputIndex)] * window[static_cast<size_t>(sample)];
        }

        fftPlan->exec_with_scratch(
            state.transform.data(), state.scratch.data(), 1.0f, pocketfft::detail::FORWARD);
        unpackSpectrum(state.transform, state.spectrum);

        for (int bin = 0; bin < numBins; ++bin)
            state.targetMagnitude[static_cast<size_t>(bin)] =
                std::abs(state.spectrum[static_cast<size_t>(bin)]);

        for (int iteration = 0; iteration < iterationCount; ++iteration)
        {
            applyTargetMagnitude(state);
            packSpectrum(state.spectrum, state.transform);
            fftPlan->exec_with_scratch(
                state.transform.data(),
                state.scratch.data(),
                1.0f / static_cast<float>(fftSize),
                pocketfft::detail::BACKWARD);

            for (int sample = 0; sample < fftSize; ++sample)
            {
                const auto outputIndex = wrapIndex(state.overlapReadIndex + sample);
                const auto overlapAdded =
                    state.overlapAddRing[static_cast<size_t>(outputIndex)]
                    + state.transform[static_cast<size_t>(sample)]
                        * synthesisWindow[static_cast<size_t>(sample)];
                state.reanalysis[static_cast<size_t>(sample)] =
                    overlapAdded * window[static_cast<size_t>(sample)];
            }

            fftPlan->exec_with_scratch(
                state.reanalysis.data(), state.scratch.data(), 1.0f, pocketfft::detail::FORWARD);
            unpackSpectrum(state.reanalysis, state.spectrum);
        }

        applyTargetMagnitude(state);
        packSpectrum(state.spectrum, state.transform);
        fftPlan->exec_with_scratch(
            state.transform.data(),
            state.scratch.data(),
            1.0f / static_cast<float>(fftSize),
            pocketfft::detail::BACKWARD);

        auto finite = true;

        for (int sample = 0; sample < fftSize; ++sample)
        {
            auto value =
                state.transform[static_cast<size_t>(sample)]
                * synthesisWindow[static_cast<size_t>(sample)];

            if (!std::isfinite(value))
            {
                value = 0.0f;
                finite = false;
            }

            const auto outputIndex = wrapIndex(state.overlapReadIndex + sample);
            state.overlapAddRing[static_cast<size_t>(outputIndex)] += value;
        }

        return finite;
    }

    static void applyTargetMagnitude(ChannelState& state) noexcept
    {
        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto index = static_cast<size_t>(bin);
            const auto magnitude = std::abs(state.spectrum[index]);
            const auto phase = magnitude > 1.0e-20f
                ? state.spectrum[index] / magnitude
                : std::complex<float> { 1.0f, 0.0f };
            state.spectrum[index] = phase * state.targetMagnitude[index];
        }

        // DC and Nyquist must remain real for a conjugate-symmetric real transform.
        state.spectrum[0] = { state.spectrum[0].real(), 0.0f };
        state.spectrum[numBins - 1] = { state.spectrum[numBins - 1].real(), 0.0f };
    }

    std::array<ChannelState, numChannels> channels {};
    SampleBlock window {};
    SampleBlock synthesisWindow {};
    std::unique_ptr<pocketfft::detail::pocketfft_r<float>> fftPlan;
    double sampleRate = 48000.0;
    float phaseMix = 0.0f;
    float phaseMixStep = 1.0f;
    int samplesUntilFrame = fftSize;
};

PhaseReconstructor::PhaseReconstructor()
    : impl(std::make_unique<Impl>())
{
}

PhaseReconstructor::~PhaseReconstructor() = default;

void PhaseReconstructor::prepare(double sampleRate)
{
    impl->prepare(sampleRate);
}

void PhaseReconstructor::reset()
{
    impl->reset();
}

bool PhaseReconstructor::process(juce::AudioBuffer<float>& buffer, bool enabled) noexcept
{
    return impl->process(buffer, enabled);
}
