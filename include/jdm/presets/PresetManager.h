#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <optional>

namespace jdm
{

// One entry in the preset browser: factory presets are built from the
// product's table and have no file; user presets are files in the product's
// user preset directory.
struct PresetInfo
{
    juce::String name, author, category, description;
    juce::StringArray tags;
    juce::File file;          // invalid for factory presets
    int factoryIndex = -1;
    bool factory = false;

    // Stable identity for the favorites list (survives rescans).
    juce::String key() const { return (factory ? "F:" : "U:") + name; }
};

// A factory preset: overrides on the default state, so tables stay valid as
// parameters evolve (a new parameter simply keeps its default).
struct FactoryPreset
{
    juce::String name, category;
    juce::String tags;        // ';'-separated
    juce::String description;
    std::vector<std::pair<juce::String, float>> values;

    // Session data the preset carries (child trees of the state, e.g. a
    // kit or a mix). A tree of an inherited type given here is not
    // inherited from the session.
    std::vector<juce::ValueTree> trees = {};
};

// Everything product-specific about the preset ecosystem, supplied once by
// each plugin (Equinox / Meridian / Zenith) at construction.
struct PresetProductConfig
{
    juce::String productName;          // "Zenith": Documents/<name>/Presets, settings file, dialogs
    juce::String extension;            // file extension without the dot, e.g. "znth"
    juce::Identifier presetTag;        // metadata wrapper root, e.g. <ZenithPreset>
    juce::Identifier stateTag;         // the APVTS root the plugin saves, e.g. <ZenithState>
    int stateVersion = 1;              // rejected whole when a file claims newer

    // Session data a loaded preset inherits when it doesn't carry its own
    // (loading a sound shouldn't silently unplug a MIDI controller):
    juce::StringArray inheritChildTrees;   // child trees (Meridian: "MIDIMAP")
    juce::StringArray inheritProperties;   // state properties (Equinox: "midimap")

    // Per-user/session UI preferences that ride in the state tree: preserved
    // from the session on load, stripped on save (Equinox: GPU/hints/range).
    juce::StringArray sessionProperties;

    // Child trees that are the session's, not a sound's: kept from the session
    // on every load (whatever the preset carries), stripped on save (Nova: the
    // song track, "SONG").
    juce::StringArray sessionChildTrees;

    juce::String factoryAuthor { "Equinox Audio" };
    std::vector<FactoryPreset> factory;
};

// Preset ecosystem shared across the Equinox Audio family (born in Meridian
// v0.8, extracted after Zenith v0.2): factory + user presets with searchable
// metadata, favorites, import/export.
//
// File format — a metadata wrapper around the state the plugin already
// saves, so presets stay plain XML (diff-, sync- and backup-friendly):
//
//   <ProductPreset stateVersion="N" name=".." author=".." category=".."
//                  tags="a;b" description="..">
//     <ProductState .../>
//   </ProductPreset>
//
// Bare-state files (a ProductState root) also load; they show up named
// after their file in the "User" category.
//
// State application and preset-name identity are delegated through
// std::functions so products with different state machinery share the same
// manager: Meridian/Zenith route through their snapshot StateManager (undo
// transactions, A/B slots), Equinox applies directly with its own migration
// callback and the default apvts-property name handling.
//
// Favorites are per-user (in an Equinox Audio settings file), deliberately
// not inside the preset files: factory presets are read-only and the same
// preset can be a favorite for one user and not another.
class PresetManager
{
public:
    using ApplyStateFn = std::function<void (const juce::ValueTree&)>;
    using GetNameFn = std::function<juce::String()>;
    using SetNameFn = std::function<void (const juce::String&)>;

    PresetManager (juce::AudioProcessorValueTreeState& s, PresetProductConfig configIn,
                   ApplyStateFn applyFn, GetNameFn getNameFn = {}, SetNameFn setNameFn = {})
        : apvts (s), productConfig (std::move (configIn)), applyState (std::move (applyFn)),
          getName (std::move (getNameFn)), setName (std::move (setNameFn)),
          favoritesFile (makeFavoritesOptions (productConfig.productName))
    {
        rescan();
    }

    const PresetProductConfig& config() const { return productConfig; }

    juce::File userPresetDirectory() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile (productConfig.productName).getChildFile ("Presets");
        dir.createDirectory();
        return dir;
    }

    // Preset identity ------------------------------------------------------------
    // By default stored as a property on the state root so it rides along
    // with sessions (and undo history where the product has one); products
    // can override both directions through the constructor callbacks.

    juce::String getCurrentPresetName() const
    {
        if (getName)
            return getName();
        return apvts.state.getProperty (presetNameProperty, juce::String()).toString();
    }

    void setCurrentPresetName (const juce::String& name)
    {
        if (setName)
            setName (name);
        else
            apvts.state.setProperty (presetNameProperty, name, nullptr);
    }

    // Scanning -----------------------------------------------------------------

    void rescan()
    {
        presets.clear();

        for (int i = 0; i < numFactoryPresets(); ++i)
        {
            const auto& f = productConfig.factory[(size_t) i];
            PresetInfo info;
            info.name = f.name;
            info.author = productConfig.factoryAuthor;
            info.category = f.category;
            info.description = f.description;
            info.tags.addTokens (f.tags, ";", {});
            info.tags.trim();
            info.factory = true;
            info.factoryIndex = i;
            presets.push_back (std::move (info));
        }

        for (const auto& entry : juce::RangedDirectoryIterator (
                 userPresetDirectory(), true, "*." + productConfig.extension))
        {
            if (auto xml = juce::parseXML (entry.getFile()))
                if (auto info = parseUserPreset (*xml, entry.getFile()))
                    presets.push_back (std::move (*info));
        }

        std::stable_sort (presets.begin(), presets.end(),
                          [] (const PresetInfo& a, const PresetInfo& b)
                          {
                              if (a.factory != b.factory)
                                  return a.factory; // factory first
                              if (a.factory)
                                  return false;     // keep table order
                              return a.name.compareIgnoreCase (b.name) < 0;
                          });
    }

    const std::vector<PresetInfo>& getPresets() const { return presets; }

    juce::StringArray getCategories() const
    {
        juce::StringArray cats;
        for (const auto& p : presets)
            if (p.category.isNotEmpty())
                cats.addIfNotAlreadyThere (p.category);
        return cats;
    }

    // Loading ------------------------------------------------------------------

    bool loadPreset (const PresetInfo& info)
    {
        juce::ValueTree state;

        if (info.factory)
        {
            if (info.factoryIndex < 0 || info.factoryIndex >= numFactoryPresets())
                return false;
            state = buildFactoryState (productConfig.factory[(size_t) info.factoryIndex]);
        }
        else
        {
            auto xml = juce::parseXML (info.file);
            if (xml == nullptr)
                return false;
            state = stateFromPresetXml (*xml);
            if (! state.isValid())
                return false;
            state.setProperty (presetNameProperty, info.name, nullptr);
        }

        // Session data the preset inherits when it carries none of its own.
        for (const auto& name : productConfig.inheritChildTrees)
        {
            const juce::Identifier type (name);
            if (! state.getChildWithName (type).isValid())
            {
                auto current = apvts.state.getChildWithName (type);
                if (current.isValid())
                    state.appendChild (current.createCopy(), nullptr);
            }
        }
        for (const auto& name : productConfig.inheritProperties)
        {
            const juce::Identifier prop (name);
            if (! state.hasProperty (prop) && apvts.state.hasProperty (prop))
                state.setProperty (prop, apvts.state.getProperty (prop), nullptr);
        }

        // UI preferences ride in the state tree for session persistence, but
        // they are the user's, not the preset's: a sound must never toggle
        // GPU rendering or resize a display.
        for (const auto& name : productConfig.sessionProperties)
        {
            const juce::Identifier prop (name);
            if (apvts.state.hasProperty (prop))
                state.setProperty (prop, apvts.state.getProperty (prop), nullptr);
        }

        for (const auto& name : productConfig.sessionChildTrees)
        {
            const juce::Identifier type (name);
            if (auto own = state.getChildWithName (type); own.isValid())
                state.removeChild (own, nullptr);
            if (auto current = apvts.state.getChildWithName (type); current.isValid())
                state.appendChild (current.createCopy(), nullptr);
        }

        applyState (state);
        return true;
    }

    // Steps through the full list in browser order (wraps around); used by
    // the header prev/next arrows.
    void loadAdjacent (bool forward)
    {
        if (presets.empty())
            return;

        const auto current = getCurrentPresetName();
        int index = -1;
        for (size_t i = 0; i < presets.size(); ++i)
            if (presets[i].name == current)
            {
                index = (int) i;
                break;
            }

        const int n = (int) presets.size();
        index = index < 0 ? (forward ? 0 : n - 1)
                          : (index + (forward ? 1 : -1) + n) % n;
        loadPreset (presets[(size_t) index]);
    }

    // Saving / deleting ----------------------------------------------------------

    bool saveUserPreset (const juce::String& name, const juce::String& author,
                         const juce::String& category, const juce::StringArray& tags,
                         const juce::String& description)
    {
        if (name.trim().isEmpty())
            return false;

        auto file = userPresetDirectory().getChildFile (
            juce::File::createLegalFileName (name.trim()) + "." + productConfig.extension);

        setCurrentPresetName (name.trim());

        auto state = apvts.copyState();
        for (const auto& propName : productConfig.sessionProperties)
            state.removeProperty (juce::Identifier (propName), nullptr);
        for (const auto& name : productConfig.sessionChildTrees)
            if (auto child = state.getChildWithName (juce::Identifier (name)); child.isValid())
                state.removeChild (child, nullptr);

        auto stateXml = state.createXml();
        if (stateXml == nullptr)
            return false;

        juce::XmlElement root (productConfig.presetTag);
        root.setAttribute ("stateVersion", productConfig.stateVersion);
        root.setAttribute ("name", name.trim());
        root.setAttribute ("author", author.trim());
        root.setAttribute ("category", category.trim().isEmpty() ? "User" : category.trim());
        root.setAttribute ("tags", tags.joinIntoString (";"));
        root.setAttribute ("description", description.trim());
        root.addChildElement (stateXml.release());

        if (! root.writeTo (file))
            return false;

        rescan();
        return true;
    }

    bool deleteUserPreset (const PresetInfo& info)
    {
        if (info.factory || ! info.file.existsAsFile())
            return false;
        if (! info.file.deleteFile())
            return false;
        rescan();
        return true;
    }

    // Import/export ----------------------------------------------------------------

    bool importPreset (const juce::File& source)
    {
        if (! source.existsAsFile())
            return false;

        auto xml = juce::parseXML (source);
        if (xml == nullptr || ! stateFromPresetXml (*xml).isValid())
            return false; // not one of this product's presets

        auto dest = userPresetDirectory().getChildFile (source.getFileName())
                        .withFileExtension (productConfig.extension);
        dest = dest.getNonexistentSibling();
        if (! source.copyFileTo (dest))
            return false;

        rescan();
        return true;
    }

    bool exportPreset (const PresetInfo& info, juce::File dest)
    {
        dest = dest.withFileExtension (productConfig.extension);

        if (! info.factory)
            return info.file.existsAsFile() && info.file.copyFileTo (dest);

        // Factory presets exist only in code — materialize one.
        if (info.factoryIndex < 0 || info.factoryIndex >= numFactoryPresets())
            return false;
        const auto& f = productConfig.factory[(size_t) info.factoryIndex];

        auto stateXml = buildFactoryState (f).createXml();
        if (stateXml == nullptr)
            return false;

        juce::XmlElement root (productConfig.presetTag);
        root.setAttribute ("stateVersion", productConfig.stateVersion);
        root.setAttribute ("name", f.name);
        root.setAttribute ("author", productConfig.factoryAuthor);
        root.setAttribute ("category", f.category);
        root.setAttribute ("tags", f.tags);
        root.setAttribute ("description", f.description);
        root.addChildElement (stateXml.release());
        return root.writeTo (dest);
    }

    // Favorites -----------------------------------------------------------------

    bool isFavorite (const PresetInfo& info) const
    {
        return getFavoriteKeys().contains (info.key());
    }

    void setFavorite (const PresetInfo& info, bool shouldBeFavorite)
    {
        auto keys = getFavoriteKeys();
        if (shouldBeFavorite)
            keys.addIfNotAlreadyThere (info.key());
        else
            keys.removeString (info.key());
        favoritesFile.setValue ("favorites", keys.joinIntoString ("|"));
        favoritesFile.saveIfNeeded();
    }

    // Preset parsing (also used by product test harnesses) -------------------------

    // Returns the wrapped state, or the tree itself for bare-state presets;
    // invalid tree if it's neither (or from a newer schema).
    static juce::ValueTree stateFromPresetXml (const juce::XmlElement& xml,
                                               const PresetProductConfig& config)
    {
        if (xml.getIntAttribute ("stateVersion", 0) > config.stateVersion)
            return {};

        if (xml.hasTagName (config.presetTag.toString()))
        {
            if (auto* stateXml = xml.getChildByName (config.stateTag.toString()))
                return juce::ValueTree::fromXml (*stateXml);
            return {};
        }

        if (xml.hasTagName (config.stateTag.toString()))
            return juce::ValueTree::fromXml (xml);

        return {};
    }

    juce::ValueTree stateFromPresetXml (const juce::XmlElement& xml) const
    {
        return stateFromPresetXml (xml, productConfig);
    }

private:
    static inline const juce::Identifier presetNameProperty { "presetName" };

    std::optional<PresetInfo> parseUserPreset (const juce::XmlElement& xml,
                                               const juce::File& file) const
    {
        if (! stateFromPresetXml (xml).isValid())
            return std::nullopt;

        PresetInfo info;
        info.file = file;

        if (xml.hasTagName (productConfig.presetTag.toString()))
        {
            info.name = xml.getStringAttribute ("name", file.getFileNameWithoutExtension());
            info.author = xml.getStringAttribute ("author");
            info.category = xml.getStringAttribute ("category", "User");
            info.description = xml.getStringAttribute ("description");
            info.tags.addTokens (xml.getStringAttribute ("tags"), ";", {});
            info.tags.trim();
            info.tags.removeEmptyStrings();
        }
        else // bare-state preset
        {
            info.name = file.getFileNameWithoutExtension();
            info.category = "User";
        }
        return info;
    }

    int numFactoryPresets() const { return (int) productConfig.factory.size(); }

    juce::ValueTree buildFactoryState (const FactoryPreset& fp) const
    {
        juce::ValueTree state (apvts.state.getType());

        for (auto* p : apvts.processor.getParameters())
        {
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            {
                float plain = rp->convertFrom0to1 (rp->getDefaultValue());
                for (const auto& [paramId, value] : fp.values)
                    if (paramId == rp->paramID)
                        plain = value;

                juce::ValueTree param ("PARAM");
                param.setProperty ("id", rp->paramID, nullptr);
                param.setProperty ("value", plain, nullptr);
                state.appendChild (param, nullptr);
            }
        }

        for (const auto& t : fp.trees)
            state.appendChild (t.createCopy(), nullptr);

        state.setProperty (presetNameProperty, fp.name, nullptr);
        return state;
    }

    // Favorites file: separate from the window/preference settings so two
    // PropertiesFile instances never clobber each other's keys.
    static juce::PropertiesFile::Options makeFavoritesOptions (const juce::String& product)
    {
        juce::PropertiesFile::Options o;
        o.applicationName = product;
        o.folderName = "Equinox Audio";
        o.filenameSuffix = "presets";
        o.osxLibrarySubFolder = "Application Support";
        return o;
    }

    juce::StringArray getFavoriteKeys() const
    {
        juce::StringArray keys;
        keys.addTokens (favoritesFile.getValue ("favorites"), "|", {});
        keys.removeEmptyStrings();
        return keys;
    }

    juce::AudioProcessorValueTreeState& apvts;
    PresetProductConfig productConfig;
    ApplyStateFn applyState;
    GetNameFn getName;
    SetNameFn setName;
    mutable juce::PropertiesFile favoritesFile;

    std::vector<PresetInfo> presets;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace jdm
