#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../presets/PresetManager.h"
#include "LookAndFeel.h"

namespace jdm
{

// Full preset browser (v0.8): search, categories, tags, favorites,
// factory/user presets, save-as with metadata, import/export.
//
// Lives as an overlay covering the whole editor content; clicking a preset
// loads it immediately (so browsing is auditioning), double-click loads and
// closes. All file dialogs are async (the tests build with
// JUCE_MODAL_LOOPS_PERMITTED=0); deleting is a two-step confirm on the
// button itself instead of a modal box.
class PresetBrowser : public juce::Component,
                      private juce::ListBoxModel
{
public:
    explicit PresetBrowser (PresetManager& pm) : manager (pm)
    {
        setWantsKeyboardFocus (true);

        title.setText ("PRESET BROWSER", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, colors::text);
        addAndMakeVisible (title);

        search.setTextToShowWhenEmpty ("Search name, tag, category...", colors::textDim);
        search.onTextChange = [this] { refreshList(); };
        search.onEscapeKey = [this] { closeBrowser(); };
        search.setTitle ("Search presets");
        search.setTooltip ("Filters the list by name, tags, category, author, and description.");
        addAndMakeVisible (search);

        closeButton.onClick = [this] { closeBrowser(); };
        closeButton.setTitle ("Close preset browser");
        closeButton.setTooltip ("Close the preset browser (Esc).");
        addAndMakeVisible (closeButton);

        categoryList.setModel (&categoryModel);
        categoryList.setRowHeight (24);
        categoryList.setColour (juce::ListBox::backgroundColourId,
                                juce::Colours::transparentBlack);
        categoryList.setTitle ("Preset categories");
        addAndMakeVisible (categoryList);

        presetList.setModel (this);
        presetList.setRowHeight (26);
        presetList.setColour (juce::ListBox::backgroundColourId,
                              juce::Colours::transparentBlack);
        presetList.setTitle ("Presets");
        addAndMakeVisible (presetList);

        info.setJustificationType (juce::Justification::topLeft);
        info.setFont (juce::Font (juce::FontOptions (12.0f)));
        info.setColour (juce::Label::textColourId, colors::textDim);
        info.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (info);

        saveAsButton.onClick = [this] { showSavePanel(); };
        saveAsButton.setTooltip ("Save the current settings as a user preset with "
                                 "name, category, tags, and description.");
        deleteButton.onClick = [this] { deleteSelected(); };
        deleteButton.setTooltip ("Delete the selected user preset (asks once; "
                                 "factory presets can't be deleted).");
        importButton.onClick = [this] { importPreset(); };
        importButton.setTooltip ("Copy a ." + manager.config().extension
                                + " preset file into your user preset folder.");
        exportButton.onClick = [this] { exportSelected(); };
        exportButton.setTooltip ("Save a copy of the selected preset to a file "
                                 "(cloud/backup friendly plain XML).");
        for (auto* b : { &saveAsButton, &deleteButton, &importButton, &exportButton })
            addAndMakeVisible (*b);

        // Save-as form ---------------------------------------------------------
        savePanel.setVisible (false);
        addChildComponent (savePanel);

        auto initField = [this] (juce::Label& label, const char* text, juce::TextEditor& ed)
        {
            label.setText (text, juce::dontSendNotification);
            label.setFont (juce::Font (juce::FontOptions (11.0f)));
            label.setColour (juce::Label::textColourId, colors::textDim);
            savePanel.addAndMakeVisible (label);
            ed.setTitle (juce::String (text) + " of the preset");
            savePanel.addAndMakeVisible (ed);
        };
        initField (nameLabel, "NAME", nameEditor);
        initField (authorLabel, "AUTHOR", authorEditor);
        initField (categoryLabel, "CATEGORY", categoryEditor);
        initField (tagsLabel, "TAGS (separate with ;)", tagsEditor);
        initField (descriptionLabel, "DESCRIPTION", descriptionEditor);
        descriptionEditor.setMultiLine (true, true);
        descriptionEditor.setReturnKeyStartsNewLine (true);

        confirmSaveButton.onClick = [this] { commitSave(); };
        cancelSaveButton.onClick = [this] { savePanel.setVisible (false); };
        savePanel.addAndMakeVisible (confirmSaveButton);
        savePanel.addAndMakeVisible (cancelSaveButton);
    }

    std::function<void()> onVisibilityChange; // parent hides/shows the scrim area

    void openBrowser()
    {
        manager.rescan();
        rebuildCategories();
        refreshList();
        selectCurrentPreset();
        setVisible (true);
        toFront (true);
        presetList.grabKeyboardFocus();
        if (onVisibilityChange)
            onVisibilityChange();
    }

    void closeBrowser()
    {
        savePanel.setVisible (false);
        setVisible (false);
        if (onVisibilityChange)
            onVisibilityChange();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colors::background.withAlpha (0.82f)); // scrim
        auto panel = panelArea().toFloat();
        g.setColour (colors::panel);
        g.fillRoundedRectangle (panel, 6.0f);
        g.setColour (colors::outline);
        g.drawRoundedRectangle (panel.reduced (0.5f), 6.0f, 1.0f);

        // Column divider.
        auto inner = panelArea().reduced (12);
        g.drawVerticalLine (inner.getX() + categoryColumnWidth + 6,
                            (float) (inner.getY() + headerHeight),
                            (float) (inner.getBottom() - footerHeight));
    }

    void resized() override
    {
        auto inner = panelArea().reduced (12);

        auto header = inner.removeFromTop (headerHeight);
        title.setBounds (header.removeFromLeft (150));
        closeButton.setBounds (header.removeFromRight (24).reduced (0, 2));
        header.removeFromRight (8);
        search.setBounds (header.reduced (8, 3));

        auto footer = inner.removeFromBottom (footerHeight);
        footer.removeFromTop (6);
        saveAsButton.setBounds (footer.removeFromLeft (78));
        footer.removeFromLeft (6);
        deleteButton.setBounds (footer.removeFromLeft (78));
        exportButton.setBounds (footer.removeFromRight (72));
        footer.removeFromRight (6);
        importButton.setBounds (footer.removeFromRight (72));

        auto infoArea = inner.removeFromBottom (infoHeight);
        info.setBounds (infoArea.reduced (4, 2));

        categoryList.setBounds (inner.removeFromLeft (categoryColumnWidth));
        inner.removeFromLeft (12);
        presetList.setBounds (inner);

        savePanel.setBounds (panelArea().withSizeKeepingCentre (380, 300));
        layoutSavePanel();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (savePanel.isVisible())
                savePanel.setVisible (false);
            else
                closeBrowser();
            return true;
        }
        if (key == juce::KeyPress::returnKey && ! savePanel.isVisible())
        {
            closeBrowser();
            return true;
        }
        return false;
    }

    // Steps used by the editor's [ / ] shortcuts when the browser is open.
    void moveSelection (int delta)
    {
        if (filtered.empty())
            return;
        const int row = juce::jlimit (0, (int) filtered.size() - 1,
                                      presetList.getSelectedRow() + delta);
        presetList.selectRow (row);
    }

private:
    // Layout constants (logical units, pre-scale).
    static constexpr int headerHeight = 34;
    static constexpr int footerHeight = 34;
    static constexpr int infoHeight = 44;
    static constexpr int categoryColumnWidth = 150;

    juce::Rectangle<int> panelArea() const
    {
        return getLocalBounds().withSizeKeepingCentre (
            juce::jmin (680, getWidth() - 40), juce::jmin (440, getHeight() - 30));
    }

    // Preset list model ----------------------------------------------------------

    int getNumRows() override { return (int) filtered.size(); }

    const PresetInfo* presetForRow (int row) const
    {
        if (row < 0 || row >= (int) filtered.size())
            return nullptr;
        return &manager.getPresets()[(size_t) filtered[(size_t) row]];
    }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        auto* p = presetForRow (row);
        if (p == nullptr)
            return;

        if (selected)
        {
            g.setColour (colors::accentDim.withAlpha (0.35f));
            g.fillRoundedRectangle (juce::Rectangle<float> (0, 1, (float) w, (float) h - 2), 3.0f);
        }

        // Favorite star, drawn in the product's control accent.
        g.setColour (manager.isFavorite (*p) ? colors::accent : colors::outline);
        g.setFont (juce::Font (juce::FontOptions ((float) h * 0.55f)));
        g.drawText (juce::String::fromUTF8 (manager.isFavorite (*p) ? "\xe2\x98\x85" : "\xe2\x98\x86"),
                    4, 0, starWidth - 4, h, juce::Justification::centred);

        g.setColour (colors::text);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (p->name, starWidth + 4, 0, w - starWidth - 130, h,
                    juce::Justification::centredLeft, true);

        g.setColour (colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (p->factory ? p->category + "  \xc2\xb7 FACTORY" : p->category,
                    w - 124, 0, 118, h, juce::Justification::centredRight, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.x < starWidth)
        {
            if (auto* p = presetForRow (row))
            {
                manager.setFavorite (*p, ! manager.isFavorite (*p));
                if (categoryList.getSelectedRow() == 1) // Favorites view
                    refreshList();
                else
                    presetList.repaintRow (row);
            }
            return;
        }
        loadRow (row);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        loadRow (row);
        closeBrowser();
    }

    void selectedRowsChanged (int row) override
    {
        // Arrow-key navigation loads as it moves (browsing = auditioning).
        if (! programmaticSelection)
            loadRow (row);
        updateInfo (row);
    }

    void returnKeyPressed (int) override { closeBrowser(); }

    void loadRow (int row)
    {
        if (auto* p = presetForRow (row))
        {
            manager.loadPreset (*p);
            updateInfo (row);
            updateButtonStates (p);
        }
    }

    void updateInfo (int row)
    {
        auto* p = presetForRow (row);
        if (p == nullptr)
        {
            info.setText ({}, juce::dontSendNotification);
            return;
        }
        juce::String text = p->description;
        juce::StringArray meta;
        if (p->author.isNotEmpty()) meta.add ("by " + p->author);
        if (! p->tags.isEmpty()) meta.add ("tags: " + p->tags.joinIntoString (", "));
        if (! meta.isEmpty())
            text << (text.isEmpty() ? "" : "\n") << meta.joinIntoString ("   \xc2\xb7   ");
        info.setText (text, juce::dontSendNotification);
        updateButtonStates (p);
    }

    void updateButtonStates (const PresetInfo* p)
    {
        deleteButton.setEnabled (p != nullptr && ! p->factory);
        deleteButton.setButtonText ("DELETE");
        deleteArmed = false;
        exportButton.setEnabled (p != nullptr);
    }

    // Category list model --------------------------------------------------------

    struct CategoryModel : juce::ListBoxModel
    {
        explicit CategoryModel (PresetBrowser& b) : owner (b) {}

        int getNumRows() override { return owner.categories.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (selected)
            {
                g.setColour (colors::accentDim.withAlpha (0.35f));
                g.fillRoundedRectangle (juce::Rectangle<float> (0, 1, (float) w, (float) h - 2), 3.0f);
            }
            g.setColour (row < 2 ? colors::text : colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (owner.categories[row], 8, 0, w - 12, h,
                        juce::Justification::centredLeft, true);
        }

        void selectedRowsChanged (int) override { owner.refreshList(); }

        PresetBrowser& owner;
    };

    void rebuildCategories()
    {
        categories.clear();
        categories.add ("All Presets");
        categories.add ("Favorites");
        categories.addArray (manager.getCategories());
        categoryList.updateContent();
        if (categoryList.getSelectedRow() < 0)
            categoryList.selectRow (0);
    }

    bool matchesFilter (const PresetInfo& p) const
    {
        const int cat = categoryList.getSelectedRow();
        if (cat == 1 && ! manager.isFavorite (p))
            return false;
        if (cat > 1 && p.category != categories[cat])
            return false;

        const auto needle = search.getText().trim();
        if (needle.isEmpty())
            return true;

        auto haystack = p.name + " " + p.category + " " + p.author + " "
                        + p.tags.joinIntoString (" ") + " " + p.description;
        return haystack.containsIgnoreCase (needle);
    }

    void refreshList()
    {
        filtered.clear();
        const auto& all = manager.getPresets();
        for (size_t i = 0; i < all.size(); ++i)
            if (matchesFilter (all[i]))
                filtered.push_back ((int) i);

        programmaticSelection = true;
        presetList.updateContent();
        presetList.deselectAllRows();
        programmaticSelection = false;
        updateInfo (-1);
    }

    void selectCurrentPreset()
    {
        const auto current = manager.getCurrentPresetName();
        for (size_t row = 0; row < filtered.size(); ++row)
        {
            if (manager.getPresets()[(size_t) filtered[row]].name == current)
            {
                programmaticSelection = true;
                presetList.selectRow ((int) row);
                programmaticSelection = false;
                updateInfo ((int) row);
                break;
            }
        }
    }

    // Actions ---------------------------------------------------------------------

    void showSavePanel()
    {
        auto* p = presetForRow (presetList.getSelectedRow());
        nameEditor.setText (manager.getCurrentPresetName(), false);
        if (p != nullptr && ! p->factory && p->name == manager.getCurrentPresetName())
        {
            authorEditor.setText (p->author, false);
            categoryEditor.setText (p->category, false);
            tagsEditor.setText (p->tags.joinIntoString (";"), false);
            descriptionEditor.setText (p->description, false);
        }
        savePanel.setVisible (true);
        savePanel.toFront (true);
        nameEditor.grabKeyboardFocus();
    }

    void commitSave()
    {
        juce::StringArray tags;
        tags.addTokens (tagsEditor.getText(), ";,", {});
        tags.trim();
        tags.removeEmptyStrings();

        if (manager.saveUserPreset (nameEditor.getText(), authorEditor.getText(),
                                    categoryEditor.getText(), tags,
                                    descriptionEditor.getText()))
        {
            savePanel.setVisible (false);
            rebuildCategories();
            refreshList();
            selectCurrentPreset();
        }
    }

    void deleteSelected()
    {
        auto* p = presetForRow (presetList.getSelectedRow());
        if (p == nullptr || p->factory)
            return;

        if (! deleteArmed) // two-step confirm, no modal loop
        {
            deleteArmed = true;
            deleteButton.setButtonText ("SURE?");
            return;
        }

        manager.deleteUserPreset (*p);
        deleteArmed = false;
        deleteButton.setButtonText ("DELETE");
        rebuildCategories();
        refreshList();
    }

    void importPreset()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Import " + manager.config().productName + " preset",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*." + manager.config().extension + ";*.xml");
        fileChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::canSelectMultipleItems,
            [this] (const juce::FileChooser& fc)
            {
                for (const auto& f : fc.getResults())
                    manager.importPreset (f);
                rebuildCategories();
                refreshList();
            });
    }

    void exportSelected()
    {
        auto* p = presetForRow (presetList.getSelectedRow());
        if (p == nullptr)
            return;
        const auto preset = *p; // copy: rescans invalidate pointers

        fileChooser = std::make_unique<juce::FileChooser> (
            "Export preset", juce::File::getSpecialLocation (
                juce::File::userDocumentsDirectory).getChildFile (
                    juce::File::createLegalFileName (preset.name)
                        + "." + manager.config().extension),
            "*." + manager.config().extension);
        fileChooser->launchAsync (
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, preset] (const juce::FileChooser& fc)
            {
                if (fc.getResult() != juce::File())
                    manager.exportPreset (preset, fc.getResult());
            });
    }

    void layoutSavePanel()
    {
        auto area = savePanel.getLocalBounds().reduced (14);
        auto field = [&area] (juce::Label& l, juce::TextEditor& e, int editorHeight)
        {
            l.setBounds (area.removeFromTop (14));
            e.setBounds (area.removeFromTop (editorHeight));
            area.removeFromTop (6);
        };
        field (nameLabel, nameEditor, 22);
        field (authorLabel, authorEditor, 22);
        field (categoryLabel, categoryEditor, 22);
        field (tagsLabel, tagsEditor, 22);
        field (descriptionLabel, descriptionEditor, 48);

        auto buttons = area.removeFromTop (24);
        confirmSaveButton.setBounds (buttons.removeFromLeft (90));
        buttons.removeFromLeft (8);
        cancelSaveButton.setBounds (buttons.removeFromLeft (90));
    }

    // The save-as sub-panel draws its own opaque card.
    struct SavePanel : juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.setColour (colors::panelRaised);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
            g.setColour (colors::outline);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
        }
    };

    static constexpr int starWidth = 26;

    PresetManager& manager;

    juce::Label title, info;
    juce::TextEditor search;
    juce::TextButton closeButton { juce::String::fromUTF8 ("\xc3\x97") };
    juce::TextButton saveAsButton { "SAVE AS" }, deleteButton { "DELETE" },
                     importButton { "IMPORT" }, exportButton { "EXPORT" };

    CategoryModel categoryModel { *this };
    juce::ListBox categoryList, presetList;
    juce::StringArray categories;
    std::vector<int> filtered;   // indices into manager.getPresets()
    bool programmaticSelection = false;
    bool deleteArmed = false;

    SavePanel savePanel;
    juce::Label nameLabel, authorLabel, categoryLabel, tagsLabel, descriptionLabel;
    juce::TextEditor nameEditor, authorEditor, categoryEditor, tagsEditor, descriptionEditor;
    juce::TextButton confirmSaveButton { "SAVE" }, cancelSaveButton { "CANCEL" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};

} // namespace jdm
