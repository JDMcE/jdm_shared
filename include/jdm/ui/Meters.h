#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "LookAndFeel.h"

namespace jdm
{

// Vertical peak level meter with UI-side decay, peak-hold line and clip
// indicator. The scale is adjustable (v0.2 groundwork for feature 12):
// setScale() takes the bottom of the range and an optional K-System
// reference level, drawn as an accent line (0 VU of the chosen K scale).
class LevelMeter : public juce::Component
{
public:
    static constexpr float noRef = -1000.0f;

    void setScale (float newMinDb, float newRefDb = noRef)
    {
        minDb = newMinDb;
        refDb = newRefDb;
        repaint();
    }

    // Feeds the peak linear gain observed since the last call.
    void push (float peakLin)
    {
        const float db = juce::Decibels::gainToDecibels (peakLin, minDb);
        displayDb = juce::jmax (db, displayDb - decayDbPerFrame);
        peakHoldDb = juce::jmax (db, peakHoldDb - holdDecayDbPerFrame);
        clipped = clipped || peakLin >= 1.0f;
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override { clipped = false; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (colors::panel);
        g.fillRoundedRectangle (area, 2.0f);

        auto inner = area.reduced (2.0f);
        auto yFor = [&inner, this] (float db)
        {
            return inner.getBottom() - (db - minDb) / -minDb * inner.getHeight();
        };

        const float norm = juce::jlimit (0.0f, 1.0f, (displayDb - minDb) / -minDb);
        auto fill = inner;
        fill = fill.removeFromBottom (fill.getHeight() * norm);
        g.setColour (colors::level);
        g.fillRoundedRectangle (fill, 1.5f);

        // Scale ticks: 6 dB spacing on zoomed/K scales, 12 dB on full.
        const int tickStep = minDb > -48.0f ? 6 : 12;
        g.setColour (colors::outline.withAlpha (0.7f));
        for (int db = -tickStep; db > (int) minDb; db -= tickStep)
            g.drawHorizontalLine ((int) yFor ((float) db), inner.getX(), inner.getRight());

        // K-System reference (0 VU) line.
        if (refDb > minDb && refDb < 0.0f)
        {
            g.setColour (colors::accent.withAlpha (0.9f));
            g.fillRect (inner.getX(), yFor (refDb) - 1.0f, inner.getWidth(), 1.6f);
        }

        // Peak hold line.
        if (peakHoldDb > minDb)
        {
            g.setColour (colors::text.withAlpha (0.9f));
            g.fillRect (inner.getX(), yFor (peakHoldDb) - 1.0f, inner.getWidth(), 1.6f);
        }

        if (clipped)
        {
            g.setColour (colors::warn);
            g.fillRoundedRectangle (inner.removeFromTop (3.0f), 1.0f);
        }

        g.setColour (colors::outline);
        g.drawRoundedRectangle (area, 2.0f, 1.0f);
    }

private:
    static constexpr float decayDbPerFrame = 1.2f;     // ~36 dB/s at 30 fps
    static constexpr float holdDecayDbPerFrame = 0.1f; // ~3 dB/s at 30 fps
    float minDb = -60.0f;
    float refDb = noRef;
    float displayDb = -60.0f;
    float peakHoldDb = -60.0f;
    bool clipped = false;
};

// Vertical gain-reduction meter: fills downward from the top, 0..24 dB,
// in the palette's reserved hue (kept away from controls). v0.2 adds a
// slow-decay peak-hold line for the deepest recent reduction (click to
// reset it immediately).
class GainReductionMeter : public juce::Component
{
public:
    void push (float grDb)
    {
        // Rise instantly, fall smoothly.
        displayGr = grDb > displayGr ? grDb
                                     : displayGr + (grDb - displayGr) * 0.35f;
        holdGr = juce::jmax (grDb, holdGr - holdDecayDbPerFrame);
        repaint();
    }

    float current() const noexcept { return displayGr; }
    float held() const noexcept { return holdGr; }

    void mouseDown (const juce::MouseEvent&) override { holdGr = 0.0f; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (colors::panel);
        g.fillRoundedRectangle (area, 2.0f);

        auto inner = area.reduced (2.0f);
        const float norm = juce::jlimit (0.0f, 1.0f, displayGr / 24.0f);
        auto fill = inner;
        fill = fill.removeFromTop (fill.getHeight() * norm);
        g.setColour (colors::reserved);
        g.fillRoundedRectangle (fill, 1.5f);

        // Scale ticks every 6 dB of GR.
        g.setColour (colors::outline.withAlpha (0.7f));
        for (int db = 6; db < 24; db += 6)
        {
            const float y = inner.getY() + (float) db / 24.0f * inner.getHeight();
            g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
        }

        // Peak-hold line for the deepest recent reduction.
        if (holdGr > 0.05f)
        {
            const float y = inner.getY()
                            + juce::jlimit (0.0f, 1.0f, holdGr / 24.0f) * inner.getHeight();
            g.setColour (colors::text.withAlpha (0.9f));
            g.fillRect (inner.getX(), y - 1.0f, inner.getWidth(), 1.6f);
        }

        g.setColour (colors::outline);
        g.drawRoundedRectangle (area, 2.0f, 1.0f);
    }

private:
    static constexpr float holdDecayDbPerFrame = 0.05f; // ~1.5 dB/s at 30 fps
    float displayGr = 0.0f;
    float holdGr = 0.0f;
};

} // namespace jdm
