#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <juce_javascript/juce_javascript.h>
#include <elem/Runtime.h>

#include "SampleLoader.h"
#include "VoiceAllocator.h"
#include "GrainScheduler.h"
#include "PhaseReconstructor.h"
#include "PresetStore.h"

#include <array>
#include <mutex>
#include <optional>

class NativeBridgeObject;


//==============================================================================
class EffectsPluginProcessor
    : public juce::AudioProcessor,
      public juce::AudioProcessorParameter::Listener,
      private juce::AsyncUpdater
{
public:
    //==============================================================================
    EffectsPluginProcessor();
    ~EffectsPluginProcessor() override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const juce::AudioProcessor::BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    /** Implement the AudioProcessorParameter::Listener interface. */
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;

    //==============================================================================
    /** Implement the AsyncUpdater interface. */
    void handleAsyncUpdate() override;

    //==============================================================================
    /** Internal helper for initializing the embedded JS engine. */
    void initJavaScriptEngine();

    /** Internal helper for propagating processor state changes. */
    void dispatchStateChange();
    void dispatchError(std::string const& name, std::string const& message);
    void openSample(const juce::File& file);
    void savePreset(const juce::String& name, bool saveAs);
    void loadPreset(const juce::String& presetId);
    void renamePreset(const juce::String& presetId, const juce::String& name);
    void deletePreset(const juce::String& presetId);

private:
    friend class NativeBridgeObject;
    void receiveSampleLoadResult(SampleLoadResult result);
    void applyPendingSampleResult();
    void registerLoadedSample();
    void registerLoadedSample(elem::Runtime<float>& targetRuntime);
    void restoreSampleFromState(const elem::js::Object& restoredState);
    int applyRuntimeInstructions(const elem::js::Array& batch);
    elem::js::Object getRuntimeSnapshot();
    void rebuildRuntime();
    struct RuntimeSlot;
    RuntimeSlot* acquireRuntime(bool tryOnly);
    static void releaseRuntime(RuntimeSlot* slot);
    elem::js::Object capturePersistentState() const;
    void applyPersistentState(const elem::js::Object& restoredState);
    void normalizeRegionParameters();
    void refreshPresetState();

    //==============================================================================
    std::atomic<bool> shouldInitialize { false };
    std::atomic<uint32_t> runtimeConfigRevision { 0 };
    std::atomic<double> lastKnownSampleRate { 0.0 };
    std::atomic<int> lastKnownBlockSize { 0 };

    elem::js::Object state;
    std::unique_ptr<juce::JavascriptEngine> jsContext;

    juce::SpinLock runtimeSwapLock;
    std::unique_ptr<RuntimeSlot> runtimeSlot;
    std::mutex sampleResultMutex;
    std::optional<SampleLoadResult> pendingSampleResult;
    juce::AudioBuffer<float> loadedSampleBuffer;
    juce::String loadedSampleResourceId;
    SampleLoader sampleLoader;
    VoiceAllocator voiceAllocator;
    GrainScheduler grainScheduler;
    PhaseReconstructor phaseReconstructor;
    PresetStore presetStore;
    juce::String activePresetId;
    std::array<const float*, VoiceAllocator::numControlChannels + GrainScheduler::numControlChannels> dspInputPointers {};
    juce::AudioParameterFloat* rootNoteParameter = nullptr;
    juce::AudioParameterFloat* transposeParameter = nullptr;
    juce::AudioParameterFloat* releaseParameter = nullptr;
    juce::AudioParameterFloat* regionStartParameter = nullptr;
    juce::AudioParameterFloat* regionEndParameter = nullptr;
    juce::AudioParameterFloat* positionParameter = nullptr;
    juce::AudioParameterFloat* scanRateParameter = nullptr;
    juce::AudioParameterBool* freezeParameter = nullptr;
    juce::AudioParameterFloat* grainSizeParameter = nullptr;
    juce::AudioParameterFloat* densityParameter = nullptr;
    juce::AudioParameterFloat* positionScatterParameter = nullptr;
    juce::AudioParameterFloat* sizeScatterParameter = nullptr;
    juce::AudioParameterFloat* pitchScatterParameter = nullptr;
    juce::AudioParameterFloat* reverseProbabilityParameter = nullptr;
    juce::AudioParameterFloat* stereoWidthParameter = nullptr;
    juce::AudioParameterFloat* attackParameter = nullptr;
    juce::AudioParameterFloat* decayParameter = nullptr;
    juce::AudioParameterFloat* sustainParameter = nullptr;
    juce::AudioParameterBool* syncEnabledParameter = nullptr;
    juce::AudioParameterChoice* densityDivisionParameter = nullptr;
    juce::AudioParameterChoice* scanDivisionParameter = nullptr;
    juce::AudioParameterBool* phaseEnabledParameter = nullptr;
    std::atomic<double> loadedSourceSampleRate { 0.0 };
    std::atomic<int64_t> loadedSourceFrames { 0 };
    std::atomic<int> activeVoiceCount { 0 };
    std::atomic<int> activeGrainCount { 0 };
    std::atomic<int> hostTempoMilliBpm { 120000 };
    std::atomic<bool> hostTempoAvailable { false };
    std::atomic<bool> hostTransportPlaying { false };
    std::atomic<bool> cpuOverload { false };
    bool phaseSafetyBypassed = false;
    int phaseRecoverySamples = 0;
    int phaseMeterSamples = 0;
    double phaseMeterSeconds = 0.0;

    //==============================================================================
    // A simple "dirty list" abstraction here for propagating realtime parameter
    // value changes
    struct ParameterReadout {
        float value = 0;
        bool dirty = false;
    };

    std::list<std::atomic<ParameterReadout>> paramReadouts;
    static_assert(std::atomic<ParameterReadout>::is_always_lock_free);

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectsPluginProcessor)
};
