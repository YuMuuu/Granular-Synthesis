#include "PluginProcessor.h"
#include "WebViewEditor.h"

#include <elem/AudioBufferResource.h>

#include <cmath>

namespace
{
constexpr int stateSchemaVersion = 1;
constexpr double fallbackTempoBpm = 120.0;

double densityDivisionQuarterNotes(int index)
{
    constexpr std::array values { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    return values[static_cast<size_t>(juce::jlimit(0, static_cast<int>(values.size()) - 1, index))];
}

double scanDivisionQuarterNotes(int index)
{
    constexpr std::array values { 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
    return values[static_cast<size_t>(juce::jlimit(0, static_cast<int>(values.size()) - 1, index))];
}

elem::js::Value parameterValueToState(const juce::AudioProcessorParameter& parameter, float normalizedValue)
{
    if (dynamic_cast<const juce::AudioParameterBool*>(&parameter) != nullptr)
        return elem::js::Value(normalizedValue >= 0.5f);

    if (auto* ranged = dynamic_cast<const juce::RangedAudioParameter*>(&parameter))
        return elem::js::Number(ranged->convertFrom0to1(normalizedValue));

    return elem::js::Number(normalizedValue);
}

std::optional<float> stateValueToNormalized(
    const juce::AudioProcessorParameter& parameter,
    const elem::js::Value& value)
{
    if (dynamic_cast<const juce::AudioParameterBool*>(&parameter) != nullptr)
    {
        if (value.isBool())
            return static_cast<bool>(value) ? 1.0f : 0.0f;

        if (value.isNumber())
            return static_cast<float>(static_cast<elem::js::Number>(value) >= 0.5);

        return std::nullopt;
    }

    if (!value.isNumber())
        return std::nullopt;

    if (auto* ranged = dynamic_cast<const juce::RangedAudioParameter*>(&parameter))
        return ranged->convertTo0to1(static_cast<float>(static_cast<elem::js::Number>(value)));

    return static_cast<float>(static_cast<elem::js::Number>(value));
}

elem::js::Object makeEmptySampleState()
{
    return {
        { "status", elem::js::String("empty") },
        { "sampleId", elem::js::String() },
        { "resourceId", elem::js::String() },
        { "fileName", elem::js::String() },
        { "originalFileName", elem::js::String() },
        { "fileHash", elem::js::String() },
        { "sampleRate", elem::js::Number(0) },
        { "numFrames", elem::js::Number(0) },
        { "durationSeconds", elem::js::Number(0) },
        { "waveformPeaks", elem::js::Array() },
        { "error", elem::js::String() }
    };
}

elem::js::Object makeSampleState(const SampleLoadResult& result, const juce::String& status)
{
    elem::js::Array peaks;
    peaks.reserve(result.waveformPeaks.size());

    for (const auto peak : result.waveformPeaks)
        peaks.emplace_back(elem::js::Number(peak));

    return {
        { "status", status.toStdString() },
        { "sampleId", result.metadata.sampleId.toStdString() },
        { "resourceId", ("sample:" + result.metadata.sampleId).toStdString() },
        { "fileName", result.metadata.fileName.toStdString() },
        { "originalFileName", result.metadata.originalFileName.toStdString() },
        { "fileHash", result.metadata.fileHash.toStdString() },
        { "sampleRate", elem::js::Number(result.metadata.sampleRate) },
        { "numFrames", elem::js::Number(result.metadata.numFrames) },
        { "durationSeconds", elem::js::Number(result.metadata.durationSeconds) },
        { "waveformPeaks", std::move(peaks) },
        { "error", result.error.toStdString() }
    };
}
}

struct EffectsPluginProcessor::RuntimeSlot
{
    RuntimeSlot(double sampleRate, int blockSize)
        : runtime(std::make_unique<elem::Runtime<float>>(sampleRate, blockSize))
    {
    }

    std::unique_ptr<elem::Runtime<float>> runtime;
    std::atomic<unsigned int> users { 0 };
};

//==============================================================================
// A quick helper for locating bundled asset files
juce::File getAssetsDirectory()
{
#if JUCE_MAC
    auto assetsDir = juce::File::getSpecialLocation(juce::File::SpecialLocationType::currentApplicationFile)
        .getChildFile("Contents/Resources/dist");
#elif JUCE_WINDOWS
    auto assetsDir = juce::File::getSpecialLocation(juce::File::SpecialLocationType::currentExecutableFile) // Plugin.vst3/Contents/<arch>/Plugin.vst3
        .getParentDirectory()  // Plugin.vst3/Contents/<arch>/
        .getParentDirectory()  // Plugin.vst3/Contents/
        .getChildFile("Resources/dist");
#else
#error "We only support Mac and Windows here yet."
#endif

    return assetsDir;
}

class NativeBridgeObject final : public juce::DynamicObject
{
public:
    explicit NativeBridgeObject (EffectsPluginProcessor& processorIn)
        : processor (processorIn)
    {
        setMethod ("postNativeMessage", [this] (const juce::var::NativeFunctionArgs& args) -> juce::var
        {
            if (args.numArguments < 1)
                return {};

            const auto batch = elem::js::parseJSON (args.arguments[0].toString().toStdString());
            const auto rc = processor.applyRuntimeInstructions(batch);

            if (rc != elem::ReturnCode::Ok())
                processor.dispatchError ("Runtime Error", elem::ReturnCode::describe (rc));

            return {};
        });

        setMethod ("log", [this] (const juce::var::NativeFunctionArgs& args) -> juce::var
        {
            juce::Array<juce::var> values;
            for (int i = 0; i < args.numArguments; ++i)
                values.add (args.arguments[i]);

            const auto payload = juce::JSON::toString (juce::var (values), false);

            if (auto* editor = static_cast<WebViewEditor*> (processor.getActiveEditor()))
            {
                const auto script = juce::String (R"script(
(function() {
  console.log(...JSON.parse(%));
  return true;
})();
)script").replace ("%", juce::JSON::toString (juce::var (payload), false)).toStdString();

                editor->getWebViewPtr()->evaluateJavascript (script);
            }
            else
            {
                DBG (payload);
            }

            return {};
        });
    }

private:
    EffectsPluginProcessor& processor;
};

//==============================================================================
EffectsPluginProcessor::EffectsPluginProcessor()
     : AudioProcessor (BusesProperties()
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
       sampleLoader([this](SampleLoadResult result) { receiveSampleLoadResult(std::move(result)); })
{
    state.insert_or_assign("schemaVersion", elem::js::Number(stateSchemaVersion));
    state.insert_or_assign("sample", makeEmptySampleState());
    state.insert_or_assign("randomSeed", elem::js::Number(grainScheduler.getRandomSeed()));
    state.insert_or_assign("presets", elem::js::Object {
        { "items", elem::js::Array() },
        { "activePresetId", elem::js::String() }
    });
    state.insert_or_assign("meters", elem::js::Object {
        { "activeVoices", elem::js::Number(0) },
        { "activeGrains", elem::js::Number(0) },
        { "cpuOverload", elem::js::Value(false) }
    });

    // Initialize parameters from the manifest file
#if ELEM_DEV_LOCALHOST
    auto manifestFile = juce::URL("http://localhost:5173/manifest.json");
    auto manifestFileContents = manifestFile.readEntireTextStream().toStdString();
#else
    auto manifestFile = getAssetsDirectory().getChildFile("manifest.json");

    if (!manifestFile.existsAsFile())
        return;

    auto manifestFileContents = manifestFile.loadFileAsString().toStdString();
#endif

    auto manifest = elem::js::parseJSON(manifestFileContents);

    if (!manifest.isObject())
        return;

    auto parameters = manifest.getWithDefault("parameters", elem::js::Array());

    for (size_t i = 0; i < parameters.size(); ++i) {
        auto descrip = parameters[i];

        if (!descrip.isObject())
            continue;

        auto type = descrip.getWithDefault("type", elem::js::String("float"));
        auto paramId = descrip.getWithDefault("paramId", elem::js::String("unknown"));
        auto name = descrip.getWithDefault("name", elem::js::String("Unknown"));
        juce::RangedAudioParameter* p = nullptr;

        if (type == elem::js::String("bool"))
        {
            auto defaultValue = descrip.getWithDefault("defaultValue", elem::js::Value(false));

            if (!defaultValue.isBool())
                continue;

            p = new juce::AudioParameterBool(
                juce::ParameterID(paramId, 1),
                name,
                static_cast<bool>(defaultValue));
        }
        else if (type == elem::js::String("choice"))
        {
            auto choicesValue = descrip.getWithDefault("choices", elem::js::Array());
            auto defaultValue = descrip.getWithDefault("defaultValue", elem::js::Number(0));

            juce::StringArray choices;

            for (const auto& choice : choicesValue)
            {
                if (!choice.isString())
                    continue;

                choices.add(juce::String(static_cast<elem::js::String>(choice)));
            }

            if (choices.isEmpty())
                continue;

            p = new juce::AudioParameterChoice(
                juce::ParameterID(paramId, 1),
                name,
                choices,
                juce::jlimit(0, choices.size() - 1, static_cast<int>(defaultValue)));
        }
        else
        {
            auto minValue = descrip.getWithDefault("min", elem::js::Number(0));
            auto maxValue = descrip.getWithDefault("max", elem::js::Number(1));
            auto interval = descrip.getWithDefault("interval", elem::js::Number(0));
            auto defaultValue = descrip.getWithDefault("defaultValue", elem::js::Number(0));

            p = new juce::AudioParameterFloat(
                juce::ParameterID(paramId, 1),
                name,
                {
                    static_cast<float>(minValue),
                    static_cast<float>(maxValue),
                    static_cast<float>(interval)
                },
                static_cast<float>(defaultValue));
        }

        if (p == nullptr)
            continue;

        p->addListener(this);
        addParameter(p);

        if (paramId == elem::js::String("rootNote"))
            rootNoteParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("transpose"))
            transposeParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("release"))
            releaseParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("regionStart"))
            regionStartParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("regionEnd"))
            regionEndParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("position"))
            positionParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("scanRate"))
            scanRateParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("freeze"))
            freezeParameter = dynamic_cast<juce::AudioParameterBool*>(p);
        else if (paramId == elem::js::String("grainSize"))
            grainSizeParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("density"))
            densityParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("positionScatter"))
            positionScatterParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("sizeScatter"))
            sizeScatterParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("pitchScatter"))
            pitchScatterParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("reverseProbability"))
            reverseProbabilityParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("stereoWidth"))
            stereoWidthParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("attack"))
            attackParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("decay"))
            decayParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("sustain"))
            sustainParameter = dynamic_cast<juce::AudioParameterFloat*>(p);
        else if (paramId == elem::js::String("syncEnabled"))
            syncEnabledParameter = dynamic_cast<juce::AudioParameterBool*>(p);
        else if (paramId == elem::js::String("densityDivision"))
            densityDivisionParameter = dynamic_cast<juce::AudioParameterChoice*>(p);
        else if (paramId == elem::js::String("scanDivision"))
            scanDivisionParameter = dynamic_cast<juce::AudioParameterChoice*>(p);
        else if (paramId == elem::js::String("phaseEnabled"))
            phaseEnabledParameter = dynamic_cast<juce::AudioParameterBool*>(p);

        const auto normalizedValue = p->getValue();
        paramReadouts.emplace_back(ParameterReadout { normalizedValue, false });
        state.insert_or_assign(
            static_cast<elem::js::String>(paramId),
            parameterValueToState(*p, normalizedValue));
    }

    refreshPresetState();
}

EffectsPluginProcessor::~EffectsPluginProcessor()
{
    sampleLoader.shutdown();

    for (auto& p : getParameters())
    {
        p->removeListener(this);
    }
}

//==============================================================================
juce::AudioProcessorEditor* EffectsPluginProcessor::createEditor()
{
    return new WebViewEditor(this, getAssetsDirectory(), 1100, 720);
}

bool EffectsPluginProcessor::hasEditor() const
{
    return true;
}

//==============================================================================
const juce::String EffectsPluginProcessor::getName() const
{
    return JucePlugin_Name;
}

bool EffectsPluginProcessor::acceptsMidi() const
{
    return true;
}

bool EffectsPluginProcessor::producesMidi() const
{
    return false;
}

bool EffectsPluginProcessor::isMidiEffect() const
{
    return false;
}

double EffectsPluginProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==============================================================================
int EffectsPluginProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int EffectsPluginProcessor::getCurrentProgram()
{
    return 0;
}

void EffectsPluginProcessor::setCurrentProgram (int /* index */) {}
const juce::String EffectsPluginProcessor::getProgramName (int /* index */) { return {}; }
void EffectsPluginProcessor::changeProgramName (int /* index */, const juce::String& /* newName */) {}

//==============================================================================
void EffectsPluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    voiceAllocator.prepare(samplesPerBlock);
    grainScheduler.prepare(samplesPerBlock, sampleRate);
    phaseReconstructor.prepare(sampleRate);
    setLatencySamples(PhaseReconstructor::latencySamples);
    phaseSafetyBypassed = false;
    phaseRecoverySamples = 0;
    phaseMeterSamples = 0;
    phaseMeterSeconds = 0.0;
    cpuOverload.store(false);

    // Some hosts call `prepareToPlay` on the real-time thread, some call it on the main thread.
    // To address the discrepancy, we check whether anything has changed since our last known
    // call. If it has, we flag for initialization of the Elementary engine and runtime, then
    // trigger an async update.
    //
    // JUCE will synchronously handle the async update if it understands
    // that we're already on the main thread.
    if (sampleRate != lastKnownSampleRate.load()
        || samplesPerBlock != lastKnownBlockSize.load())
    {
        runtimeConfigRevision.fetch_add(1, std::memory_order_acq_rel);
        lastKnownSampleRate.store(sampleRate, std::memory_order_relaxed);
        lastKnownBlockSize.store(samplesPerBlock, std::memory_order_relaxed);
        runtimeConfigRevision.fetch_add(1, std::memory_order_release);

        shouldInitialize.store(true);
    }

    // Now that the environment is set up, push our current state
    triggerAsyncUpdate();
}

void EffectsPluginProcessor::releaseResources()
{
    voiceAllocator.reset();
    grainScheduler.reset();
    phaseReconstructor.reset();
    activeVoiceCount.store(0);
    activeGrainCount.store(0);
}

bool EffectsPluginProcessor::isBusesLayoutSupported (const AudioProcessor::BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet().isDisabled()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void EffectsPluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Clear the output buffer to prevent any garbage if our runtime isn't ready
    buffer.clear();

    // Process the elementary runtime
    if (rootNoteParameter != nullptr
        && transposeParameter != nullptr
        && releaseParameter != nullptr
        && regionStartParameter != nullptr
        && regionEndParameter != nullptr
        && positionParameter != nullptr
        && scanRateParameter != nullptr
        && freezeParameter != nullptr
        && grainSizeParameter != nullptr
        && densityParameter != nullptr
        && positionScatterParameter != nullptr
        && sizeScatterParameter != nullptr
        && pitchScatterParameter != nullptr
        && reverseProbabilityParameter != nullptr
        && stereoWidthParameter != nullptr
        && attackParameter != nullptr
        && decayParameter != nullptr
        && sustainParameter != nullptr
        && syncEnabledParameter != nullptr
        && densityDivisionParameter != nullptr
        && scanDivisionParameter != nullptr
        && phaseEnabledParameter != nullptr
        && buffer.getNumSamples() <= voiceAllocator.getMaximumBlockSize()
        && buffer.getNumSamples() <= grainScheduler.getMaximumBlockSize())
    {
        auto tempoBpm = fallbackTempoBpm;
        auto tempoAvailable = false;
        auto transportPlaying = false;

        if (auto* playHead = getPlayHead())
        {
            if (const auto position = playHead->getPosition())
            {
                transportPlaying = position->getIsPlaying();

                if (const auto bpm = position->getBpm())
                {
                    if (std::isfinite(*bpm) && *bpm > 0.0)
                    {
                        tempoBpm = *bpm;
                        tempoAvailable = true;
                    }
                }
            }
        }

        const auto tempoMilliBpm = static_cast<int>(std::round(tempoBpm * 1000.0));
        const auto tempoChanged = hostTempoMilliBpm.exchange(tempoMilliBpm) != tempoMilliBpm;
        const auto availabilityChanged =
            hostTempoAvailable.exchange(tempoAvailable) != tempoAvailable;
        const auto playingChanged =
            hostTransportPlaying.exchange(transportPlaying) != transportPlaying;
        const auto transportStateChanged = tempoChanged || availabilityChanged || playingChanged;

        if (transportStateChanged)
            triggerAsyncUpdate();

        voiceAllocator.render(
            midiMessages,
            buffer.getNumSamples(),
            rootNoteParameter->get(),
            transposeParameter->get(),
            releaseParameter->get(),
            lastKnownSampleRate.load());

        const auto newActiveVoiceCount = voiceAllocator.getActiveVoiceCount();

        if (activeVoiceCount.exchange(newActiveVoiceCount) != newActiveVoiceCount)
            triggerAsyncUpdate();

        GrainScheduler::Parameters grainParameters;
        grainParameters.regionStart = regionStartParameter->get();
        grainParameters.regionEnd = regionEndParameter->get();
        grainParameters.position = juce::jlimit(
            grainParameters.regionStart,
            juce::jmax(grainParameters.regionStart, grainParameters.regionEnd),
            positionParameter->get());
        grainParameters.scanRate = scanRateParameter->get();
        grainParameters.freeze = freezeParameter->get();
        grainParameters.grainSizeMs = grainSizeParameter->get();
        grainParameters.densityHz = densityParameter->get();
        grainParameters.positionScatter = positionScatterParameter->get();
        grainParameters.sizeScatter = sizeScatterParameter->get();
        grainParameters.pitchScatterSemitones = pitchScatterParameter->get();
        grainParameters.reverseProbability = reverseProbabilityParameter->get();
        grainParameters.stereoWidth = stereoWidthParameter->get();
        grainParameters.attackSeconds = attackParameter->get();
        grainParameters.decaySeconds = decayParameter->get();
        grainParameters.sustain = sustainParameter->get();
        grainParameters.releaseSeconds = releaseParameter->get();

        if (syncEnabledParameter->get())
        {
            const auto quarterNotesPerSecond = tempoBpm / 60.0;
            grainParameters.densityHz = static_cast<float>(
                quarterNotesPerSecond
                / densityDivisionQuarterNotes(densityDivisionParameter->getIndex()));
            grainParameters.scanRate = static_cast<float>(
                quarterNotesPerSecond
                / scanDivisionQuarterNotes(scanDivisionParameter->getIndex()));
        }

        grainScheduler.render(
            voiceAllocator.getChannelPointers(),
            VoiceAllocator::numControlChannels,
            buffer.getNumSamples(),
            VoiceAllocator::maxVoices,
            VoiceAllocator::triggerOffset,
            VoiceAllocator::gateOffset,
            VoiceAllocator::pitchOffset,
            VoiceAllocator::velocityOffset,
            grainParameters,
            loadedSourceSampleRate.load(),
            loadedSourceFrames.load());

        const auto newActiveGrainCount = grainScheduler.getActiveGrainCount();

        if (activeGrainCount.exchange(newActiveGrainCount) != newActiveGrainCount)
            triggerAsyncUpdate();

        std::copy_n(
            voiceAllocator.getChannelPointers(),
            VoiceAllocator::numControlChannels,
            dspInputPointers.begin());
        std::copy_n(
            grainScheduler.getChannelPointers(),
            GrainScheduler::numControlChannels,
            dspInputPointers.begin() + VoiceAllocator::numControlChannels);

        if (auto* runtimeLease = acquireRuntime(true))
        {
            const juce::ScopeGuard releaseLease {
                [this, runtimeLease] { releaseRuntime(runtimeLease); }
            };
            runtimeLease->runtime->process(
                dspInputPointers.data(),
                dspInputPointers.size(),
                const_cast<float**>(buffer.getArrayOfWritePointers()),
                buffer.getNumChannels(),
                buffer.getNumSamples(),
                nullptr
            );
        }
    }

    const auto phaseRequested = phaseEnabledParameter != nullptr && phaseEnabledParameter->get();
    const auto phaseStartTicks = juce::Time::getHighResolutionTicks();
    const auto finite = phaseReconstructor.process(
        buffer,
        phaseRequested && !phaseSafetyBypassed);
    const auto phaseElapsedSeconds =
        juce::Time::highResolutionTicksToSeconds(
            juce::Time::getHighResolutionTicks() - phaseStartTicks);

    if (!phaseRequested)
    {
        phaseSafetyBypassed = false;
        phaseRecoverySamples = 0;
        phaseMeterSamples = 0;
        phaseMeterSeconds = 0.0;

        if (cpuOverload.exchange(false))
            triggerAsyncUpdate();
    }
    else if (!finite)
    {
        phaseSafetyBypassed = true;
        phaseRecoverySamples = 0;
        phaseMeterSamples = 0;
        phaseMeterSeconds = 0.0;

        if (!cpuOverload.exchange(true))
            triggerAsyncUpdate();
    }
    else if (phaseSafetyBypassed)
    {
        phaseRecoverySamples += buffer.getNumSamples();

        if (phaseRecoverySamples >= static_cast<int>(lastKnownSampleRate.load()))
        {
            phaseSafetyBypassed = false;
            phaseRecoverySamples = 0;
            phaseMeterSamples = 0;
            phaseMeterSeconds = 0.0;
        }
    }
    else
    {
        phaseMeterSamples += buffer.getNumSamples();
        phaseMeterSeconds += phaseElapsedSeconds;

        if (phaseMeterSamples >= PhaseReconstructor::fftSize * 2)
        {
            const auto availableSeconds = phaseMeterSamples / lastKnownSampleRate.load();
            const auto exceededBudget = phaseMeterSeconds > availableSeconds * 0.9;
            phaseMeterSamples = 0;
            phaseMeterSeconds = 0.0;

            if (exceededBudget)
            {
                phaseSafetyBypassed = true;
                phaseRecoverySamples = 0;

                if (!cpuOverload.exchange(true))
                    triggerAsyncUpdate();
            }
            else if (cpuOverload.exchange(false))
            {
                triggerAsyncUpdate();
            }
        }
    }
}

void EffectsPluginProcessor::parameterValueChanged (int parameterIndex, float newValue)
{
    // Mark the updated parameter value in the dirty list
    auto& pr = *std::next(paramReadouts.begin(), parameterIndex);

    pr.store({ newValue, true });
    triggerAsyncUpdate();
}

void EffectsPluginProcessor::parameterGestureChanged (int, bool)
{
    // Not implemented
}

//==============================================================================
void EffectsPluginProcessor::handleAsyncUpdate()
{
    if (shouldInitialize.exchange(false))
        rebuildRuntime();

    applyPendingSampleResult();
    normalizeRegionParameters();

    state.insert_or_assign("meters", elem::js::Object {
        { "activeVoices", elem::js::Number(activeVoiceCount.load()) },
        { "activeGrains", elem::js::Number(activeGrainCount.load()) },
        { "cpuOverload", elem::js::Value(cpuOverload.load()) }
    });
    state.insert_or_assign("transport", elem::js::Object {
        { "bpm", elem::js::Number(hostTempoMilliBpm.load() / 1000.0) },
        { "tempoAvailable", elem::js::Value(hostTempoAvailable.load()) },
        { "isPlaying", elem::js::Value(hostTransportPlaying.load()) }
    });

    // Next we iterate over the current parameter values to update our local state
    // object, which we in turn dispatch into the JavaScript engine
    auto& params = getParameters();

    // Reduce over the changed parameters to resolve our updated processor state
    for (size_t i = 0; i < paramReadouts.size(); ++i)
    {
        // We atomically exchange an arbitrary value with a dirty flag false, because
        // we know that the next time we exchange, if the dirty flag is still false, the
        // value can be considered arbitrary. Only when we exchange and find the dirty flag
        // true do we consider the value as having been written by the processor since
        // we last looked.
        auto& current = *std::next(paramReadouts.begin(), i);
        auto pr = current.exchange({0.0f, false});

        if (pr.dirty)
        {
            if (auto* parameterWithId = dynamic_cast<juce::AudioProcessorParameterWithID*>(params[i]))
            {
                state.insert_or_assign(
                    parameterWithId->paramID.toStdString(),
                    parameterValueToState(*params[i], pr.value));
            }
        }
    }

    dispatchStateChange();
}

void EffectsPluginProcessor::openSample(const juce::File& file)
{
    auto sampleState = makeEmptySampleState();
    sampleState.insert_or_assign("status", elem::js::String("loading"));
    sampleState.insert_or_assign("originalFileName", file.getFileName().toStdString());
    state.insert_or_assign("sample", std::move(sampleState));
    dispatchStateChange();
    sampleLoader.importFile(file);
}

void EffectsPluginProcessor::savePreset(const juce::String& name, bool saveAs)
{
    juce::String savedPresetId;
    const auto presetId = saveAs ? juce::String() : activePresetId;
    const auto result = presetStore.save(
        presetId,
        name,
        capturePersistentState(),
        grainScheduler.getRandomSeed(),
        savedPresetId);

    if (result.failed())
    {
        dispatchError("Preset Save Error", result.getErrorMessage().toStdString());
        return;
    }

    activePresetId = savedPresetId;
    refreshPresetState();
    dispatchStateChange();
}

void EffectsPluginProcessor::loadPreset(const juce::String& presetId)
{
    juce::String error;
    const auto preset = presetStore.load(presetId, error);

    if (!preset.has_value())
    {
        dispatchError("Preset Load Error", error.toStdString());
        refreshPresetState();
        dispatchStateChange();
        return;
    }

    grainScheduler.setRandomSeed(preset->randomSeed);
    applyPersistentState(preset->state);
    activePresetId = presetId;
    refreshPresetState();
    triggerAsyncUpdate();
}

void EffectsPluginProcessor::renamePreset(
    const juce::String& presetId,
    const juce::String& name)
{
    const auto result = presetStore.rename(presetId, name);

    if (result.failed())
        dispatchError("Preset Rename Error", result.getErrorMessage().toStdString());

    refreshPresetState();
    dispatchStateChange();
}

void EffectsPluginProcessor::deletePreset(const juce::String& presetId)
{
    const auto result = presetStore.remove(presetId);

    if (result.failed())
    {
        dispatchError("Preset Delete Error", result.getErrorMessage().toStdString());
    }
    else if (activePresetId == presetId)
    {
        activePresetId.clear();
    }

    refreshPresetState();
    dispatchStateChange();
}

void EffectsPluginProcessor::receiveSampleLoadResult(SampleLoadResult result)
{
    {
        const std::scoped_lock lock(sampleResultMutex);
        pendingSampleResult = std::move(result);
    }

    triggerAsyncUpdate();
}

void EffectsPluginProcessor::applyPendingSampleResult()
{
    std::optional<SampleLoadResult> result;

    {
        const std::scoped_lock lock(sampleResultMutex);
        result = std::exchange(pendingSampleResult, std::nullopt);
    }

    if (!result.has_value())
        return;

    if (!result->succeeded())
    {
        const auto currentSampleIt = state.find("sample");

        if (result->metadata.sampleId.isNotEmpty()
            && currentSampleIt != state.end()
            && currentSampleIt->second.isObject())
        {
            auto missingState = currentSampleIt->second.getObject();
            missingState.insert_or_assign("status", elem::js::String("missing"));
            missingState.insert_or_assign("error", result->error.toStdString());
            state.insert_or_assign("sample", std::move(missingState));
        }
        else
        {
            state.insert_or_assign("sample", makeSampleState(*result, "error"));
        }

        dispatchError("Sample Load Error", result->error.toStdString());
        return;
    }

    const auto currentSampleIt = state.find("sample");

    if (result->metadata.originalFileName.isEmpty()
        && currentSampleIt != state.end()
        && currentSampleIt->second.isObject())
    {
        const auto currentSample = currentSampleIt->second.getObject();
        const auto originalNameIt = currentSample.find("originalFileName");

        if (originalNameIt != currentSample.end() && originalNameIt->second.isString())
            result->metadata.originalFileName = static_cast<elem::js::String>(originalNameIt->second);
    }

    loadedSampleBuffer = std::move(result->monoBuffer);
    loadedSampleResourceId = "sample:" + result->metadata.sampleId;
    loadedSourceSampleRate.store(result->metadata.sampleRate);
    loadedSourceFrames.store(result->metadata.numFrames);
    registerLoadedSample();
    state.insert_or_assign("sample", makeSampleState(*result, "ready"));
}

void EffectsPluginProcessor::registerLoadedSample()
{
    if (loadedSampleBuffer.getNumSamples() == 0 || loadedSampleResourceId.isEmpty())
        return;

    const auto resourceId = loadedSampleResourceId.toStdString();
    auto resource = std::make_unique<elem::AudioBufferResource>(
        loadedSampleBuffer.getWritePointer(0),
        static_cast<size_t>(loadedSampleBuffer.getNumSamples()));
    if (auto* runtimeLease = acquireRuntime(false))
    {
        const juce::ScopeGuard releaseLease {
            [this, runtimeLease] { releaseRuntime(runtimeLease); }
        };
        runtimeLease->runtime->addSharedResource(resourceId, std::move(resource));
    }
}

void EffectsPluginProcessor::registerLoadedSample(elem::Runtime<float>& targetRuntime)
{
    if (loadedSampleBuffer.getNumSamples() == 0 || loadedSampleResourceId.isEmpty())
        return;

    targetRuntime.addSharedResource(
        loadedSampleResourceId.toStdString(),
        std::make_unique<elem::AudioBufferResource>(
            loadedSampleBuffer.getWritePointer(0),
            static_cast<size_t>(loadedSampleBuffer.getNumSamples())));
}

int EffectsPluginProcessor::applyRuntimeInstructions(const elem::js::Array& batch)
{
    auto* runtimeLease = acquireRuntime(false);

    if (runtimeLease == nullptr)
        return elem::ReturnCode::Ok();

    const juce::ScopeGuard releaseLease {
        [this, runtimeLease] { releaseRuntime(runtimeLease); }
    };
    return runtimeLease->runtime->applyInstructions(batch);
}

elem::js::Object EffectsPluginProcessor::getRuntimeSnapshot()
{
    auto* runtimeLease = acquireRuntime(false);

    if (runtimeLease == nullptr)
        return {};

    const juce::ScopeGuard releaseLease {
        [this, runtimeLease] { releaseRuntime(runtimeLease); }
    };
    return runtimeLease->runtime->snapshot();
}

void EffectsPluginProcessor::rebuildRuntime()
{
    double sampleRate = 0.0;
    int blockSize = 0;
    uint32_t revisionBefore = 0;
    uint32_t revisionAfter = 0;

    do
    {
        revisionBefore = runtimeConfigRevision.load(std::memory_order_acquire);

        if ((revisionBefore & 1u) != 0)
        {
            juce::Thread::yield();
            continue;
        }

        sampleRate = lastKnownSampleRate.load(std::memory_order_relaxed);
        blockSize = lastKnownBlockSize.load(std::memory_order_relaxed);
        revisionAfter = runtimeConfigRevision.load(std::memory_order_acquire);
    }
    while (revisionBefore != revisionAfter || (revisionAfter & 1u) != 0);

    if (sampleRate <= 0.0 || blockSize <= 0)
        return;

    auto nextRuntime = std::make_unique<RuntimeSlot>(sampleRate, blockSize);
    registerLoadedSample(*nextRuntime->runtime);
    std::unique_ptr<RuntimeSlot> retiredRuntime;

    {
        const juce::SpinLock::ScopedLockType runtimeGuard(runtimeSwapLock);
        retiredRuntime = std::move(runtimeSlot);
        runtimeSlot = std::move(nextRuntime);
    }

    while (retiredRuntime != nullptr && retiredRuntime->users.load() != 0)
        juce::Thread::yield();

    retiredRuntime.reset();
    initJavaScriptEngine();
}

EffectsPluginProcessor::RuntimeSlot* EffectsPluginProcessor::acquireRuntime(bool tryOnly)
{
    if (tryOnly)
    {
        const juce::SpinLock::ScopedTryLockType runtimeGuard(runtimeSwapLock);

        if (!runtimeGuard.isLocked() || runtimeSlot == nullptr)
            return nullptr;

        runtimeSlot->users.fetch_add(1, std::memory_order_acquire);
        return runtimeSlot.get();
    }

    const juce::SpinLock::ScopedLockType runtimeGuard(runtimeSwapLock);

    if (runtimeSlot == nullptr)
        return nullptr;

    runtimeSlot->users.fetch_add(1, std::memory_order_acquire);
    return runtimeSlot.get();
}

void EffectsPluginProcessor::releaseRuntime(RuntimeSlot* slot)
{
    slot->users.fetch_sub(1, std::memory_order_release);
}

void EffectsPluginProcessor::restoreSampleFromState(const elem::js::Object& restoredState)
{
    const auto sampleIt = restoredState.find("sample");

    if (sampleIt == restoredState.end() || !sampleIt->second.isObject())
        return;

    const auto sampleObject = sampleIt->second.getObject();
    const auto idIt = sampleObject.find("sampleId");

    if (idIt == sampleObject.end() || !idIt->second.isString())
        return;

    const auto sampleId = juce::String(static_cast<elem::js::String>(idIt->second));

    if (sampleId.isEmpty())
        return;

    auto loadingState = sampleObject;
    loadingState.insert_or_assign("status", elem::js::String("loading"));
    loadingState.insert_or_assign("error", elem::js::String());
    state.insert_or_assign("sample", std::move(loadingState));
    sampleLoader.restoreSample(sampleId);
}

elem::js::Object EffectsPluginProcessor::capturePersistentState() const
{
    elem::js::Object persistentState {
        { "schemaVersion", elem::js::Number(stateSchemaVersion) },
        { "randomSeed", elem::js::Number(grainScheduler.getRandomSeed()) }
    };
    const auto sampleIt = state.find("sample");

    if (sampleIt != state.end())
        persistentState.insert_or_assign("sample", sampleIt->second);

    for (auto* parameter : getParameters())
    {
        if (auto* parameterWithId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter))
        {
            persistentState.insert_or_assign(
                parameterWithId->paramID.toStdString(),
                parameterValueToState(*parameter, parameter->getValue()));
        }
    }

    return persistentState;
}

void EffectsPluginProcessor::applyPersistentState(const elem::js::Object& restoredState)
{
    for (auto* parameter : getParameters())
    {
        auto* parameterWithId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter);

        if (parameterWithId == nullptr)
            continue;

        const auto id = parameterWithId->paramID.toStdString();
        const auto it = restoredState.find(id);

        if (it == restoredState.end())
            continue;

        if (auto normalizedValue = stateValueToNormalized(*parameter, it->second))
        {
            parameter->setValueNotifyingHost(*normalizedValue);
            state.insert_or_assign(id, parameterValueToState(*parameter, *normalizedValue));
        }
    }

    const auto seedIt = restoredState.find("randomSeed");

    if (seedIt != restoredState.end() && seedIt->second.isNumber())
        grainScheduler.setRandomSeed(
            static_cast<uint32_t>(static_cast<elem::js::Number>(seedIt->second)));

    state.insert_or_assign("randomSeed", elem::js::Number(grainScheduler.getRandomSeed()));
    normalizeRegionParameters();
    restoreSampleFromState(restoredState);
}

void EffectsPluginProcessor::normalizeRegionParameters()
{
    if (regionStartParameter == nullptr
        || regionEndParameter == nullptr
        || positionParameter == nullptr)
        return;

    auto regionStart = regionStartParameter->get();
    auto regionEnd = regionEndParameter->get();

    if (regionStart > regionEnd)
    {
        regionStart = regionEnd;
        regionStartParameter->setValueNotifyingHost(
            regionStartParameter->convertTo0to1(regionStart));
    }

    const auto position = positionParameter->get();
    const auto constrainedPosition = juce::jlimit(regionStart, regionEnd, position);

    if (position != constrainedPosition)
    {
        positionParameter->setValueNotifyingHost(
            positionParameter->convertTo0to1(constrainedPosition));
    }
}

void EffectsPluginProcessor::refreshPresetState()
{
    elem::js::Array items;

    for (const auto& preset : presetStore.list())
    {
        items.emplace_back(elem::js::Object {
            { "id", preset.id.toStdString() },
            { "name", preset.name.toStdString() },
            { "modifiedAt", preset.modifiedAt.toStdString() }
        });
    }

    state.insert_or_assign("presets", elem::js::Object {
        { "items", std::move(items) },
        { "activePresetId", activePresetId.toStdString() }
    });
}

void EffectsPluginProcessor::initJavaScriptEngine()
{
    jsContext = std::make_unique<juce::JavascriptEngine>();

    auto* bridge = new NativeBridgeObject (*this);
    jsContext->registerNativeObject ("__native", bridge);

    // Install some native interop functions in our JavaScript environment
    jsContext->execute (R"shim(
(function() {
  globalThis.__postNativeMessage__ = function(payload) {
    return __native.postNativeMessage(payload);
  };

  globalThis.__log__ = function(...args) {
    return __native.log(JSON.stringify(args));
  };
})();
)shim");

    // A simple shim to write various console operations to our native __log__ handler
    jsContext->execute(R"shim(
(function() {
  if (typeof globalThis.console === 'undefined') {
    globalThis.console = {
      log(...args) {
        __log__('[embedded:log]', ...args);
      },
      warn(...args) {
          __log__('[embedded:warn]', ...args);
      },
      error(...args) {
          __log__('[embedded:error]', ...args);
      }
    };
  }
})();
    )shim");

    // Load and evaluate our Elementary js main file
#if ELEM_DEV_LOCALHOST
    auto dspEntryFile = juce::URL("http://localhost:5173/dsp.main.js");
    auto dspEntryFileContents = dspEntryFile.readEntireTextStream().toStdString();
#else
    auto dspEntryFile = getAssetsDirectory().getChildFile("dsp.main.js");

    if (!dspEntryFile.existsAsFile())
        return;

    auto dspEntryFileContents = dspEntryFile.loadFileAsString().toStdString();
#endif
    jsContext->execute(dspEntryFileContents);

    // Re-hydrate from current state
    const auto* kHydrateScript = R"script(
(function() {
  if (typeof globalThis.__receiveHydrationData__ !== 'function')
    return false;

  globalThis.__receiveHydrationData__(%);
  return true;
})();
)script";

    auto expr = juce::String(kHydrateScript)
        .replace("%", elem::js::serialize(elem::js::serialize(getRuntimeSnapshot())))
        .toStdString();
    jsContext->execute(expr);
}

void EffectsPluginProcessor::dispatchStateChange()
{
    const auto* kDispatchScript = R"script(
(function() {
  if (typeof globalThis.__receiveStateChange__ !== 'function')
    return false;

  globalThis.__receiveStateChange__(%);
  return true;
})();
)script";

    // Need the double serialize here to correctly form the string script. The first
    // serialize produces the payload we want, the second serialize ensures we can replace
    // the % character in the above block and produce a valid javascript expression.
    auto localState = state;
    localState.insert_or_assign("sampleRate", lastKnownSampleRate.load());

    auto expr = juce::String(kDispatchScript).replace("%", elem::js::serialize(elem::js::serialize(localState))).toStdString();

    // First we try to dispatch to the UI if it's available, because running this step will
    // just involve placing a message in a queue.
    if (auto* editor = static_cast<WebViewEditor*>(getActiveEditor())) {
        editor->getWebViewPtr()->evaluateJavascript(expr);
    }

    // Next we dispatch to the local engine which will evaluate any necessary JavaScript synchronously
    // here on the main thread
    if (jsContext != nullptr)
        jsContext->execute(expr);
}

void EffectsPluginProcessor::dispatchError(std::string const& name, std::string const& message)
{
    const auto* kDispatchScript = R"script(
(function() {
  if (typeof globalThis.__receiveError__ !== 'function')
    return false;

  let e = new Error(%);
  e.name = @;

  globalThis.__receiveError__(e);
  return true;
})();
)script";

    // Need the serialize here to correctly form the string script.
    auto expr = juce::String(kDispatchScript).replace("@", elem::js::serialize(name)).replace("%", elem::js::serialize(message)).toStdString();

    // First we try to dispatch to the UI if it's available, because running this step will
    // just involve placing a message in a queue.
    if (auto* editor = static_cast<WebViewEditor*>(getActiveEditor())) {
        editor->getWebViewPtr()->evaluateJavascript(expr);
    }

    // Next we dispatch to the local engine which will evaluate any necessary JavaScript synchronously
    // here on the main thread
    if (jsContext != nullptr)
        jsContext->execute(expr);
}

//==============================================================================
void EffectsPluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto currentState = capturePersistentState();
    auto serialized = elem::js::serialize(currentState);
    destData.replaceAll((void *) serialized.c_str(), serialized.size());
}

void EffectsPluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    try {
        auto str = std::string(static_cast<const char*>(data), sizeInBytes);
        auto parsed = elem::js::parseJSON(str);
        auto o = parsed.getObject();
        applyPersistentState(o);
        activePresetId.clear();
        refreshPresetState();
        triggerAsyncUpdate();
    } catch(...) {
        // Failed to parse the incoming state, or the state we did parse was not actually
        // an object type. How you handle it is up to you, here we just ignore it
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EffectsPluginProcessor();
}
