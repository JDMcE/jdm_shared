#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace jdm
{

// Design language shared across the Equinox Audio family (locked at
// Meridian v0.2; extracted into jdm_shared after Zenith v0.2): near-black
// blue-grey surfaces, one control accent, quiet typography in caps for
// section labels. Each plugin installs its own palette pair at startup —
// same hue roles, product-specific accent (Equinox = cyan, Meridian =
// amber, Zenith = green) plus a "reserved" hue kept away from controls
// (gain reduction in the dynamics plugins, the summed curve in the EQ).
//
// Accessibility: the palette entries are mutable so the whole UI can swap
// to a high-contrast variant at runtime (same hue roles, raised luminance
// separation). Every paint routine reads these globals, so a swap +
// repaint restyles everything. The palette is process-wide state —
// multiple open editors of one product follow the same user setting by
// design (and each plugin binary carries its own copy of these globals,
// so sibling products in one host never fight over them).
namespace colors
{
    struct Palette
    {
        juce::Colour background, panel, panelRaised, outline,
                     text, textDim, accent, accentDim,
                     reserved, level, warn, sidechain;
    };

    // Neutral placeholders until a product installs its palettes (the
    // family blue-grey surfaces with a grey accent, so a missing install
    // is visually obvious without being unusable).
    inline Palette standardPalette {
        juce::Colour (0xff15181e), juce::Colour (0xff1d2129),
        juce::Colour (0xff242a34), juce::Colour (0xff313845),
        juce::Colour (0xffd8dce4), juce::Colour (0xff828a9a),
        juce::Colour (0xff9aa4b4), juce::Colour (0xff5a6474),
        juce::Colour (0xffe09a44), juce::Colour (0xff46b5dc),
        juce::Colour (0xffd96a5a), juce::Colour (0xffb287d8)
    };

    inline Palette highContrastPalette {
        juce::Colour (0xff090b0e), juce::Colour (0xff11141a),
        juce::Colour (0xff1b2029), juce::Colour (0xff5a6478),
        juce::Colour (0xfff4f7fc), juce::Colour (0xffbcc5d6),
        juce::Colour (0xffc4cede), juce::Colour (0xff7a8494),
        juce::Colour (0xffffb45c), juce::Colour (0xff62cdf4),
        juce::Colour (0xffff8071), juce::Colour (0xffcfa5f4)
    };

    inline juce::Colour background   = standardPalette.background;
    inline juce::Colour panel        = standardPalette.panel;
    inline juce::Colour panelRaised  = standardPalette.panelRaised;
    inline juce::Colour outline      = standardPalette.outline;

    inline juce::Colour text         = standardPalette.text;
    inline juce::Colour textDim      = standardPalette.textDim;

    inline juce::Colour accent       = standardPalette.accent;     // controls (the product's family accent)
    inline juce::Colour accentDim    = standardPalette.accentDim;
    inline juce::Colour reserved     = standardPalette.reserved;   // the hue kept away from controls
    inline juce::Colour level        = standardPalette.level;      // level meters
    inline juce::Colour warn         = standardPalette.warn;       // clip indicators
    inline juce::Colour sidechain    = standardPalette.sidechain;  // external sidechain trace

    inline bool highContrast = false;

    inline void setHighContrast (bool on)
    {
        highContrast = on;
        const auto& p = on ? highContrastPalette : standardPalette;
        background = p.background; panel = p.panel; panelRaised = p.panelRaised;
        outline = p.outline; text = p.text; textDim = p.textDim;
        accent = p.accent; accentDim = p.accentDim; reserved = p.reserved;
        level = p.level; warn = p.warn; sidechain = p.sidechain;
    }

    // Called by each product (the LookAndFeel constructor does it) before
    // anything paints. Idempotent.
    inline void install (const Palette& standard, const Palette& highContrastVariant)
    {
        standardPalette = standard;
        highContrastPalette = highContrastVariant;
        setHighContrast (highContrast); // re-derive the active globals
    }
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        applyPalette();
    }

    // Product entry point: installs the product's palette pair, then binds
    // the component colour ids.
    LookAndFeel (const colors::Palette& standard, const colors::Palette& highContrastVariant)
    {
        colors::install (standard, highContrastVariant);
        applyPalette();
    }

    // Re-reads the (possibly swapped) palette into the component colour ids.
    // Call after colors::setHighContrast, then sendLookAndFeelChange + repaint.
    void applyPalette()
    {
        setColour (juce::ResizableWindow::backgroundColourId, colors::background);
        setColour (juce::Label::textColourId, colors::text);

        setColour (juce::Slider::backgroundColourId, colors::panelRaised);
        setColour (juce::Slider::trackColourId, colors::accentDim);
        setColour (juce::Slider::thumbColourId, colors::accent);
        setColour (juce::Slider::textBoxTextColourId, colors::textDim);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, colors::accentDim);

        setColour (juce::ComboBox::backgroundColourId, colors::panelRaised);
        setColour (juce::ComboBox::textColourId, colors::text);
        setColour (juce::ComboBox::outlineColourId, colors::outline);
        setColour (juce::ComboBox::arrowColourId, colors::textDim);

        setColour (juce::PopupMenu::backgroundColourId, colors::panelRaised);
        setColour (juce::PopupMenu::textColourId, colors::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, colors::accentDim);
        setColour (juce::PopupMenu::highlightedTextColourId, colors::text);

        setColour (juce::TextButton::buttonColourId, colors::panelRaised);
        setColour (juce::TextButton::buttonOnColourId, colors::accentDim);
        setColour (juce::TextButton::textColourOffId, colors::text);
        setColour (juce::TextButton::textColourOnId, colors::text);

        setColour (juce::ToggleButton::textColourId, colors::text);
        setColour (juce::ToggleButton::tickColourId, colors::accent);
        setColour (juce::ToggleButton::tickDisabledColourId, colors::outline);

        setColour (juce::TextEditor::backgroundColourId, colors::panel);
        setColour (juce::TextEditor::textColourId, colors::text);
        setColour (juce::TextEditor::outlineColourId, colors::outline);

        setColour (juce::TooltipWindow::backgroundColourId, colors::panelRaised);
        setColour (juce::TooltipWindow::textColourId, colors::text);

        setColour (juce::ListBox::backgroundColourId, colors::panel);
        setColour (juce::ListBox::textColourId, colors::text);
        setColour (juce::ScrollBar::thumbColourId, colors::outline);
        setColour (juce::CaretComponent::caretColourId, colors::accent);
    }

    // Accessibility text scaling: every stock text path funnels through
    // these font getters, so one factor scales the whole UI's type.
    void setTextScale (float scale) { textScale = juce::jlimit (1.0f, 1.5f, scale); }
    float getTextScale() const { return textScale; }

    juce::Font getLabelFont (juce::Label& label) override
    {
        auto f = LookAndFeel_V4::getLabelFont (label);
        return f.withHeight (juce::jmin (f.getHeight(), 14.0f) * textScale);
    }

    juce::Font getTextButtonFont (juce::TextButton& b, int buttonHeight) override
    {
        return LookAndFeel_V4::getTextButtonFont (b, buttonHeight).withHeight (
            juce::jmin (13.0f, (float) buttonHeight * 0.7f) * textScale);
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return LookAndFeel_V4::getComboBoxFont (box).withHeight (13.0f * textScale);
    }

    juce::Font getPopupMenuFont() override
    {
        return LookAndFeel_V4::getPopupMenuFont().withHeight (14.0f * textScale);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y,
                                              (float) width, (float) height)
                          .reduced (4.0f);
        const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto centre = bounds.getCentre();
        const float radius = size * 0.5f;
        const float arcThickness = juce::jmax (2.0f, radius * 0.11f);
        const float arcRadius = radius - arcThickness * 0.5f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // Body.
        const float bodyRadius = arcRadius - arcThickness * 1.4f;
        g.setColour (colors::panelRaised);
        g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius,
                       bodyRadius * 2.0f, bodyRadius * 2.0f);
        g.setColour (colors::outline);
        g.drawEllipse (centre.x - bodyRadius, centre.y - bodyRadius,
                       bodyRadius * 2.0f, bodyRadius * 2.0f, 1.0f);

        // Track + value arcs.
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (colors::outline);
        g.strokePath (track, juce::PathStrokeType (arcThickness,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             rotaryStartAngle, angle, true);
        g.setColour (colors::accent);
        g.strokePath (value, juce::PathStrokeType (arcThickness,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Pointer.
        juce::Path pointer;
        pointer.addRoundedRectangle (-arcThickness * 0.5f, -bodyRadius,
                                     arcThickness, bodyRadius * 0.42f, arcThickness * 0.5f);
        g.setColour (colors::text);
        g.fillPath (pointer, juce::AffineTransform::rotation (angle)
                                 .translated (centre.x, centre.y));

        // Keyboard focus ring (arrow keys nudge the focused knob).
        if (slider.hasKeyboardFocus (true))
        {
            g.setColour (colors::accent.withAlpha (0.85f));
            g.drawEllipse (centre.x - radius, centre.y - radius,
                           radius * 2.0f, radius * 2.0f, 1.2f);
        }
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        LookAndFeel_V4::drawButtonBackground (g, button, backgroundColour,
                                              highlighted, down);
        if (button.hasKeyboardFocus (true))
        {
            g.setColour (colors::accent.withAlpha (0.85f));
            g.drawRoundedRectangle (button.getLocalBounds().toFloat().reduced (0.8f),
                                    3.0f, 1.2f);
        }
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox& box) override
    {
        LookAndFeel_V4::drawComboBox (g, width, height, isButtonDown,
                                      buttonX, buttonY, buttonW, buttonH, box);
        if (box.hasKeyboardFocus (true))
        {
            g.setColour (colors::accent.withAlpha (0.85f));
            g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width,
                                                            (float) height).reduced (0.8f),
                                    3.0f, 1.2f);
        }
    }

private:
    float textScale = 1.0f;
};

} // namespace jdm
