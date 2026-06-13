#include "PresetStore.h"

#include <elem/JSON.h>

#include <algorithm>

namespace
{
elem::js::Object readPresetObject(const juce::File& file)
{
    const auto value = elem::js::parseJSON(file.loadFileAsString().toStdString());

    if (!value.isObject())
        throw std::runtime_error("The preset file is not a JSON object.");

    return value.getObject();
}

juce::String readString(
    const elem::js::Object& object,
    const char* key,
    const juce::String& fallback = {})
{
    const auto it = object.find(key);
    return it != object.end() && it->second.isString()
        ? juce::String(static_cast<elem::js::String>(it->second))
        : fallback;
}

juce::Result writePresetObject(const juce::File& target, const elem::js::Object& object)
{
    juce::TemporaryFile temporaryFile(target);

    if (!temporaryFile.getFile().replaceWithText(elem::js::serialize(object)))
        return juce::Result::fail("The temporary preset file could not be written.");

    if (!temporaryFile.overwriteTargetFileWithTemporary())
        return juce::Result::fail("The preset file could not be replaced.");

    return juce::Result::ok();
}
}

juce::File PresetStore::getPresetStorageDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Granular Synthesis")
        .getChildFile("Presets");
}

std::vector<PresetSummary> PresetStore::list() const
{
    std::vector<PresetSummary> presets;
    const auto directory = getPresetStorageDirectory();

    if (!directory.isDirectory())
        return presets;

    for (const auto& file : directory.findChildFiles(juce::File::findFiles, false, "*.json"))
    {
        try
        {
            const auto object = readPresetObject(file);
            const auto id = readString(object, "presetId", file.getFileNameWithoutExtension());
            const auto name = readString(object, "name");

            if (id.isNotEmpty() && name.isNotEmpty())
                presets.push_back({ id, name, readString(object, "modifiedAt") });
        }
        catch (...)
        {
            // Invalid preset files are omitted from the user-facing list.
        }
    }

    std::sort(presets.begin(), presets.end(), [](const auto& left, const auto& right)
    {
        const auto nameOrder = left.name.compareNatural(right.name);
        return nameOrder == 0 ? left.id < right.id : nameOrder < 0;
    });
    return presets;
}

juce::Result PresetStore::save(
    const juce::String& presetId,
    const juce::String& name,
    const elem::js::Object& state,
    uint32_t randomSeed,
    juce::String& savedPresetId) const
{
    const auto validName = validateName(name);

    if (validName.isEmpty())
        return juce::Result::fail("Enter a preset name.");

    if (presetId.isNotEmpty() && !isValidPresetId(presetId))
        return juce::Result::fail("The preset ID is invalid.");

    const auto directory = getPresetStorageDirectory();

    if (!directory.createDirectory())
        return juce::Result::fail("The preset storage directory could not be created.");

    savedPresetId = presetId.isNotEmpty() ? presetId : juce::Uuid().toString();
    const auto target = getPresetFile(savedPresetId);
    auto createdAt = juce::Time::getCurrentTime().toISO8601(true);

    if (target.existsAsFile())
    {
        try
        {
            createdAt = readString(readPresetObject(target), "createdAt", createdAt);
        }
        catch (...)
        {
        }
    }

    const auto modifiedAt = juce::Time::getCurrentTime().toISO8601(true);
    const elem::js::Object object {
        { "schemaVersion", elem::js::Number(schemaVersion) },
        { "presetId", savedPresetId.toStdString() },
        { "name", validName.toStdString() },
        { "createdAt", createdAt.toStdString() },
        { "modifiedAt", modifiedAt.toStdString() },
        { "randomSeed", elem::js::Number(randomSeed) },
        { "state", state }
    };
    return writePresetObject(target, object);
}

std::optional<PresetLoadResult> PresetStore::load(
    const juce::String& presetId,
    juce::String& error) const
{
    if (!isValidPresetId(presetId))
    {
        error = "The preset ID is invalid.";
        return std::nullopt;
    }

    const auto file = getPresetFile(presetId);

    if (!file.existsAsFile())
    {
        error = "The selected preset is missing.";
        return std::nullopt;
    }

    try
    {
        const auto object = readPresetObject(file);
        const auto versionIt = object.find("schemaVersion");
        const auto stateIt = object.find("state");

        if (versionIt == object.end()
            || !versionIt->second.isNumber()
            || static_cast<int>(static_cast<elem::js::Number>(versionIt->second)) != schemaVersion)
        {
            error = "The preset uses an unsupported schema version.";
            return std::nullopt;
        }

        if (stateIt == object.end() || !stateIt->second.isObject())
        {
            error = "The preset does not contain a valid state.";
            return std::nullopt;
        }

        uint32_t randomSeed = 0x47525359u;
        const auto seedIt = object.find("randomSeed");

        if (seedIt != object.end() && seedIt->second.isNumber())
            randomSeed = static_cast<uint32_t>(static_cast<elem::js::Number>(seedIt->second));

        return PresetLoadResult { stateIt->second.getObject(), randomSeed };
    }
    catch (...)
    {
        error = "The preset file could not be parsed.";
        return std::nullopt;
    }
}

juce::Result PresetStore::rename(const juce::String& presetId, const juce::String& name) const
{
    const auto validName = validateName(name);

    if (validName.isEmpty())
        return juce::Result::fail("Enter a preset name.");

    if (!isValidPresetId(presetId))
        return juce::Result::fail("The preset ID is invalid.");

    const auto file = getPresetFile(presetId);

    if (!file.existsAsFile())
        return juce::Result::fail("The selected preset is missing.");

    try
    {
        auto object = readPresetObject(file);
        object.insert_or_assign("name", elem::js::String(validName.toStdString()));
        object.insert_or_assign(
            "modifiedAt",
            elem::js::String(juce::Time::getCurrentTime().toISO8601(true).toStdString()));
        return writePresetObject(file, object);
    }
    catch (...)
    {
        return juce::Result::fail("The preset file could not be parsed.");
    }
}

juce::Result PresetStore::remove(const juce::String& presetId) const
{
    if (!isValidPresetId(presetId))
        return juce::Result::fail("The preset ID is invalid.");

    const auto file = getPresetFile(presetId);

    if (!file.existsAsFile())
        return juce::Result::fail("The selected preset is missing.");

    return file.deleteFile()
        ? juce::Result::ok()
        : juce::Result::fail("The preset file could not be deleted.");
}

juce::File PresetStore::getPresetFile(const juce::String& presetId)
{
    return getPresetStorageDirectory().getChildFile(presetId + ".json");
}

bool PresetStore::isValidPresetId(const juce::String& presetId)
{
    return presetId.length() == 36
        && presetId.retainCharacters("0123456789abcdefABCDEF-") == presetId;
}

juce::String PresetStore::validateName(const juce::String& name)
{
    return name.trim().substring(0, 80);
}
