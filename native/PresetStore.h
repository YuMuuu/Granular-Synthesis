#pragma once

#include <juce_core/juce_core.h>

#include <elem/Value.h>

#include <optional>
#include <vector>

struct PresetSummary
{
    juce::String id;
    juce::String name;
    juce::String modifiedAt;
};

struct PresetLoadResult
{
    elem::js::Object state;
    uint32_t randomSeed = 0;
};

class PresetStore
{
public:
    static constexpr int schemaVersion = 1;

    static juce::File getPresetStorageDirectory();

    std::vector<PresetSummary> list() const;
    juce::Result save(
        const juce::String& presetId,
        const juce::String& name,
        const elem::js::Object& state,
        uint32_t randomSeed,
        juce::String& savedPresetId) const;
    std::optional<PresetLoadResult> load(const juce::String& presetId, juce::String& error) const;
    juce::Result rename(const juce::String& presetId, const juce::String& name) const;
    juce::Result remove(const juce::String& presetId) const;

private:
    static juce::File getPresetFile(const juce::String& presetId);
    static bool isValidPresetId(const juce::String& presetId);
    static juce::String validateName(const juce::String& name);
};
