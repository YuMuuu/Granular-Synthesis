#include "PluginProcessor.h"
#include "WebViewEditor.h"

#include <cstddef>
#include <cstring>
#include <unordered_map>


double numberFromVar(const juce::var& v)
{
    return static_cast<double>(v);
}

juce::String getMimeType(const juce::String& ext)
{
    static const std::unordered_map<juce::String, juce::String> mimeTypes {
        { ".html",   "text/html" },
        { ".js",     "application/javascript" },
        { ".css",    "text/css" },
        { ".json",   "application/json" },
        { ".svg",    "image/svg+xml" },
        { ".png",    "image/png" },
        { ".jpg",    "image/jpeg" },
        { ".jpeg",   "image/jpeg" },
        { ".woff2",  "font/woff2" },
    };

    if (auto it = mimeTypes.find(ext.toLowerCase()); it != mimeTypes.end())
        return it->second;

    return "application/octet-stream";
}

std::vector<std::byte> toByteVector(const juce::MemoryBlock& block)
{
    std::vector<std::byte> result(block.getSize());
    std::memcpy(result.data(), block.getData(), block.getSize());
    return result;
}

//==============================================================================
WebViewEditor::WebViewEditor(juce::AudioProcessor* proc, juce::File const& assets, int width, int height)
    : juce::AudioProcessorEditor(proc),
      assetDirectory(assets)
{
    setSize(width, height);

    const auto nativeBridgeScript = juce::String(R"script(
(function() {
  globalThis.__postNativeMessage__ = function(message, payload) {
    window.__JUCE__.backend.emitEvent("postNativeMessage", [message, payload ?? null]);
  };
})();
)script");

    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled()
        .withUserScript(nativeBridgeScript)
        .withEventListener("postNativeMessage", [this](const juce::var& args) {
            handleNativeMessage(args);
        })
        .withResourceProvider([this](const juce::String& path) {
            return getResource(path);
        }
#if ELEM_DEV_LOCALHOST
        , juce::URL("http://localhost:5173").getOrigin()
#endif
        );

#if JUCE_WINDOWS
    options = options
        .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}
            .withUserDataFolder(juce::File::getSpecialLocation(juce::File::SpecialLocationType::tempDirectory)));
#endif

    webView = std::make_unique<juce::WebBrowserComponent>(options);
    addAndMakeVisible(*webView);
    webView->setBounds(getLocalBounds());

#if ELEM_DEV_LOCALHOST
    webView->goToURL("http://localhost:5173");
#else
    webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
#endif
}

juce::WebBrowserComponent* WebViewEditor::getWebViewPtr()
{
    return webView.get();
}

void WebViewEditor::paint (juce::Graphics& g)
{
    juce::ignoreUnused(g);
}

void WebViewEditor::resized()
{
    if (webView != nullptr)
        webView->setBounds(getLocalBounds());
}

//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> WebViewEditor::getResource(const juce::String& path) const
{
    auto relPath = path == "/" ? juce::String("index.html") : path.trimCharactersAtStart("/");
    auto f = assetDirectory.getChildFile(relPath);
    juce::MemoryBlock mb;

    if (!f.existsAsFile() || !f.loadFileAsData(mb))
        return {};

    return juce::WebBrowserComponent::Resource {
        toByteVector(mb),
        getMimeType(f.getFileExtension())
    };
}

void WebViewEditor::handleNativeMessage(const juce::var& args)
{
    const auto* array = args.getArray();

    if (array == nullptr || array->isEmpty())
        return;

    const auto eventName = array->getReference(0).toString();

    // When the webView loads it should send a message telling us that it has established
    // its message-passing hooks and is ready for a state dispatch.
    if (eventName == "ready")
    {
        if (auto* ptr = dynamic_cast<EffectsPluginProcessor*>(getAudioProcessor()))
            ptr->dispatchStateChange();
    }

#if ELEM_DEV_LOCALHOST
    if (eventName == "reload")
    {
        if (auto* ptr = dynamic_cast<EffectsPluginProcessor*>(getAudioProcessor()))
        {
            ptr->initJavaScriptEngine();
            ptr->dispatchStateChange();
        }
    }
#endif

    if (eventName == "setParameterValue" && array->size() > 1)
        handleSetParameterValueEvent(array->getReference(1));

    if (eventName == "openSample")
        openSampleChooser();

    if (array->size() <= 1)
        return;

    const auto* payload = array->getReference(1).getDynamicObject();

    if (payload == nullptr)
        return;

    auto* processor = dynamic_cast<EffectsPluginProcessor*>(getAudioProcessor());

    if (processor == nullptr)
        return;

    if (eventName == "savePreset")
        processor->savePreset(payload->getProperty("name").toString(), false);
    else if (eventName == "savePresetAs")
        processor->savePreset(payload->getProperty("name").toString(), true);
    else if (eventName == "loadPreset")
        processor->loadPreset(payload->getProperty("presetId").toString());
    else if (eventName == "renamePreset")
        processor->renamePreset(
            payload->getProperty("presetId").toString(),
            payload->getProperty("name").toString());
    else if (eventName == "deletePreset")
        processor->deletePreset(payload->getProperty("presetId").toString());
}

void WebViewEditor::handleSetParameterValueEvent(const juce::var& e)
{
    auto* obj = e.getDynamicObject();

    if (obj == nullptr)
        return;

    const auto paramId = obj->getProperty("paramId").toString();
    auto value = juce::jlimit(0.0f, 1.0f, static_cast<float>(
        numberFromVar(obj->getProperty("value"))));
    juce::RangedAudioParameter* target = nullptr;
    juce::RangedAudioParameter* regionStart = nullptr;
    juce::RangedAudioParameter* regionEnd = nullptr;
    juce::RangedAudioParameter* position = nullptr;

    for (auto& p : getAudioProcessor()->getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
        {
            if (ranged->paramID == paramId)
                target = ranged;
            else if (ranged->paramID == "regionStart")
                regionStart = ranged;
            else if (ranged->paramID == "regionEnd")
                regionEnd = ranged;
            else if (ranged->paramID == "position")
                position = ranged;
        }
    }

    if (target == nullptr)
        return;

    if (paramId == "regionStart" && regionEnd != nullptr)
        value = juce::jmin(value, regionEnd->getValue());
    else if (paramId == "regionEnd" && regionStart != nullptr)
        value = juce::jmax(value, regionStart->getValue());
    else if (paramId == "position" && regionStart != nullptr && regionEnd != nullptr)
        value = juce::jlimit(regionStart->getValue(), regionEnd->getValue(), value);

    target->setValueNotifyingHost(value);

    if (position == nullptr)
        return;

    if (paramId == "regionStart" && position->getValue() < value)
        position->setValueNotifyingHost(value);
    else if (paramId == "regionEnd" && position->getValue() > value)
        position->setValueNotifyingHost(value);
}

void WebViewEditor::openSampleChooser()
{
    sampleFileChooser = std::make_unique<juce::FileChooser>(
        "Open WAV",
        juce::File(),
        "*.wav",
        true);

    sampleFileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = SafePointer<WebViewEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            const auto selectedFile = chooser.getResult();

            if (selectedFile == juce::File())
                return;

            if (auto* processor = dynamic_cast<EffectsPluginProcessor*>(safeThis->getAudioProcessor()))
                processor->openSample(selectedFile);
        });
}
