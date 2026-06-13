#include "PluginProcessor.h"
#include "WebViewEditor.h"

#include <elem/AudioBufferResource.h>

namespace
{
constexpr int stateSchemaVersion = 1;

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
            const auto rc = processor.runtime->applyInstructions (batch);

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

        const auto normalizedValue = p->getValue();
        paramReadouts.emplace_back(ParameterReadout { normalizedValue, false });
        state.insert_or_assign(
            static_cast<elem::js::String>(paramId),
            parameterValueToState(*p, normalizedValue));
    }
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
    // Some hosts call `prepareToPlay` on the real-time thread, some call it on the main thread.
    // To address the discrepancy, we check whether anything has changed since our last known
    // call. If it has, we flag for initialization of the Elementary engine and runtime, then
    // trigger an async update.
    //
    // JUCE will synchronously handle the async update if it understands
    // that we're already on the main thread.
    if (sampleRate != lastKnownSampleRate || samplesPerBlock != lastKnownBlockSize) {
        lastKnownSampleRate = sampleRate;
        lastKnownBlockSize = samplesPerBlock;

        shouldInitialize.store(true);
    }

    // Now that the environment is set up, push our current state
    triggerAsyncUpdate();
}

void EffectsPluginProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool EffectsPluginProcessor::isBusesLayoutSupported (const AudioProcessor::BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet().isDisabled()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void EffectsPluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /* midiMessages */)
{
    // Clear the output buffer to prevent any garbage if our runtime isn't ready
    buffer.clear();

    // Process the elementary runtime
    if (runtime != nullptr) {
        runtime->process(
            nullptr,
            0,
            const_cast<float**>(buffer.getArrayOfWritePointers()),
            buffer.getNumChannels(),
            buffer.getNumSamples(),
            nullptr
        );
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
    // First things first, we check the flag to identify if we should initialize the Elementary
    // runtime and engine.
    if (shouldInitialize.exchange(false)) {
        // TODO: This is definitely not thread-safe! It could delete a Runtime instance while
        // the real-time thread is using it. Depends on when the host will call prepareToPlay.
        runtime = std::make_unique<elem::Runtime<float>>(lastKnownSampleRate, lastKnownBlockSize);
        registerLoadedSample();
        initJavaScriptEngine();
    }

    applyPendingSampleResult();

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
    registerLoadedSample();
    state.insert_or_assign("sample", makeSampleState(*result, "ready"));
}

void EffectsPluginProcessor::registerLoadedSample()
{
    if (runtime == nullptr || loadedSampleBuffer.getNumSamples() == 0 || loadedSampleResourceId.isEmpty())
        return;

    runtime->addSharedResource(
        loadedSampleResourceId.toStdString(),
        std::make_unique<elem::AudioBufferResource>(
            loadedSampleBuffer.getWritePointer(0),
            static_cast<size_t>(loadedSampleBuffer.getNumSamples())));
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

    auto expr = juce::String(kHydrateScript).replace("%", elem::js::serialize(elem::js::serialize(runtime->snapshot()))).toStdString();
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
    localState.insert_or_assign("sampleRate", lastKnownSampleRate);

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
    auto currentState = state;
    currentState.insert_or_assign("schemaVersion", elem::js::Number(stateSchemaVersion));

    for (auto* parameter : getParameters())
    {
        if (auto* parameterWithId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter))
        {
            currentState.insert_or_assign(
                parameterWithId->paramID.toStdString(),
                parameterValueToState(*parameter, parameter->getValue()));
        }
    }

    auto serialized = elem::js::serialize(currentState);
    destData.replaceAll((void *) serialized.c_str(), serialized.size());
}

void EffectsPluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    try {
        auto str = std::string(static_cast<const char*>(data), sizeInBytes);
        auto parsed = elem::js::parseJSON(str);
        auto o = parsed.getObject();

        for (auto* parameter : getParameters())
        {
            auto* parameterWithId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter);

            if (parameterWithId == nullptr)
                continue;

            const auto id = parameterWithId->paramID.toStdString();
            const auto it = o.find(id);

            if (it == o.end())
                continue;

            if (auto normalizedValue = stateValueToNormalized(*parameter, it->second))
            {
                parameter->setValueNotifyingHost(*normalizedValue);
                state.insert_or_assign(id, parameterValueToState(*parameter, *normalizedValue));
            }
        }

        restoreSampleFromState(o);
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
