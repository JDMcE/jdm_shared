#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace jdm
{

// Noise gate with two ways of opening:
//
//  * threshold — the classic detector: opens when the (stereo-linked) peak
//    rises above the threshold, closes 4 dB below it (hysteresis), after the
//    hold. Without lookahead the attack eats a little of the transient.
//  * keyed — the caller says *when* a hit lands ("onsets", sample offsets in
//    the block). An instrument knows its own notes before they sound, so the
//    gate opens on a linear ramp that reaches full level exactly at the
//    onset: nothing of the transient is lost, and bleed never false-triggers
//    it. Onsets may lie beyond the block (up to the attack time ahead) and may
//    be passed again in the next block: the state depends only on where each
//    onset lies, so repeats are harmless.
//
// Closed, the gate attenuates by `rangeDb` (not to silence by default, which
// is how drum gates are set). After the hold it releases exponentially.
class Gate
{
public:
    struct Params
    {
        bool keyed = false;
        float thresholdDb = -40.0f;
        float rangeDb = -40.0f;   // gain when closed
        float attackMs = 0.5f;
        float holdMs = 50.0f;
        float releaseMs = 80.0f;

        bool operator== (const Params&) const = default;
    };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        configured = false;
        setParams (params);
        reset();
    }

    void reset() noexcept
    {
        env = floor;
        hold = 0;
        open = false;
    }

    void setParams (const Params& p) noexcept
    {
        if (configured && p == params)
            return;
        params = p;
        configured = true;
        floor = juce::Decibels::decibelsToGain (juce::jlimit (-90.0f, 0.0f, p.rangeDb), -90.0f);
        attackSamples = juce::jmax (0, (int) std::lround (fs * 0.001 * juce::jmax (0.0f, p.attackMs)));
        holdSamples = (int) std::lround (fs * 0.001 * juce::jmax (0.0f, p.holdMs));
        releaseCoeff = std::exp (-1.0f / (float) (fs * 0.001 * juce::jmax (1.0f, p.releaseMs)));
        attackCoeff = std::exp (-1.0f / (float) (fs * 0.001 * juce::jmax (0.05f, p.attackMs)));
        openLevel = juce::Decibels::decibelsToGain (p.thresholdDb);
        closeLevel = juce::Decibels::decibelsToGain (p.thresholdDb - 4.0f);
        env = juce::jmax (env, floor);
    }

    // The furthest ahead (samples) an onset matters: the attack ramp.
    int getAttackSamples() const noexcept { return attackSamples; }

    // In place; `right` may be null. `onsets` (keyed mode): ascending sample
    // offsets relative to this block, any value (past ones are ignored).
    void process (float* left, float* right, int n, const int* onsets = nullptr, int numOnsets = 0) noexcept
    {
        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            if (params.keyed)
            {
                while (k < numOnsets && onsets[k] < i)
                    ++k;
                const int until = k < numOnsets ? onsets[k] - i : -1;
                if (until >= 0 && until <= attackSamples)
                {
                    env = juce::jmax (env, 1.0f - (float) until / (float) (attackSamples + 1));
                    if (until == 0)
                    {
                        env = 1.0f;
                        hold = holdSamples;
                    }
                }
                else if (hold > 0)
                    --hold;
                else
                    env = floor + (env - floor) * releaseCoeff;
            }
            else
            {
                float peak = std::abs (left[i]);
                if (right != nullptr)
                    peak = juce::jmax (peak, std::abs (right[i]));

                if (peak > openLevel)
                {
                    open = true;
                    hold = holdSamples;
                }
                else if (open && peak < closeLevel)
                {
                    if (hold > 0)
                        --hold;
                    else
                        open = false;
                }

                env = open ? 1.0f + (env - 1.0f) * attackCoeff
                           : floor + (env - floor) * releaseCoeff;
            }

            left[i] *= env;
            if (right != nullptr)
                right[i] *= env;
        }
    }

    float currentGain() const noexcept { return env; }

private:
    Params params;
    bool configured = false, open = false;
    double fs = 48000.0;
    float floor = 0.01f, env = 0.01f;
    float releaseCoeff = 0.0f, attackCoeff = 0.0f, openLevel = 0.01f, closeLevel = 0.006f;
    int attackSamples = 24, holdSamples = 2400, hold = 0;
};

} // namespace jdm
