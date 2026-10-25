#pragma once

#include "Biquad.h"

namespace jdm
{

// Channel-strip EQ: high-pass and low-pass cuts plus four bands (low shelf,
// two bells, high shelf), mono or stereo. The console-style subset of
// Equinox: no dynamic bands, no linear phase, nothing that adds latency.
//
// Inert when nothing is switched on (no sections, no cost). Coefficients are
// redesigned only when a setting changes; a band switching on starts from
// clean state.
class ChannelEq
{
public:
    struct Band
    {
        bool on = false;
        float freq = 1000.0f;
        float gainDb = 0.0f;
        float q = 0.707f;

        bool operator== (const Band&) const = default;
    };

    struct Params
    {
        bool hpOn = false;
        float hpFreq = 60.0f;
        int hpOrder = 2;           // 12 dB/oct
        bool lpOn = false;
        float lpFreq = 18000.0f;
        int lpOrder = 2;
        Band low  { false, 100.0f, 0.0f, 0.707f };   // shelf
        Band lowMid  { false, 400.0f, 0.0f, 1.0f };  // bell
        Band highMid { false, 3000.0f, 0.0f, 1.0f }; // bell
        Band high { false, 8000.0f, 0.0f, 0.707f };  // shelf

        bool operator== (const Params&) const = default;
    };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        designed = false;
        reset();
    }

    void reset() noexcept
    {
        hp.reset();
        lp.reset();
        bands.reset();
    }

    void setParams (const Params& p) noexcept
    {
        if (designed && p == params)
            return;

        const bool hpWasOn = designed && params.hpOn, lpWasOn = designed && params.lpOn;
        const bool bandsWereOn = designed && anyBand (params);
        params = p;
        designed = true;

        if (p.hpOn)
            hp.setButterworth (p.hpFreq, sampleRate, p.hpOrder, false);
        else
            hp.clearStages();
        if (p.lpOn)
            lp.setButterworth (p.lpFreq, sampleRate, p.lpOrder, true);
        else
            lp.clearStages();

        bands.clearStages();
        const double nyq = sampleRate * 0.49;
        auto w0 = [&] (float f) { return 2.0 * biquad::pi * juce::jlimit (10.0, nyq, (double) f) / sampleRate; };
        auto q = [] (float v) { return juce::jlimit (0.1, 18.0, (double) v); };
        if (p.low.on)     bands.addStage (biquad::shelf (w0 (p.low.freq), q (p.low.q), p.low.gainDb, false));
        if (p.lowMid.on)  bands.addStage (biquad::bell (w0 (p.lowMid.freq), q (p.lowMid.q), p.lowMid.gainDb));
        if (p.highMid.on) bands.addStage (biquad::bell (w0 (p.highMid.freq), q (p.highMid.q), p.highMid.gainDb));
        if (p.high.on)    bands.addStage (biquad::shelf (w0 (p.high.freq), q (p.high.q), p.high.gainDb, true));

        // A section that has just appeared starts from silence rather than
        // from whatever the previous design left in its state.
        if (p.hpOn && ! hpWasOn) hp.reset();
        if (p.lpOn && ! lpWasOn) lp.reset();
        if (anyBand (p) && ! bandsWereOn) bands.reset();
    }

    bool isActive() const noexcept
    {
        return hp.getNumStages() + lp.getNumStages() + bands.getNumStages() > 0;
    }

    // `right` may be null (mono).
    void process (float* left, float* right, int n) noexcept
    {
        for (auto* c : { &hp, &lp })
            if (c->getNumStages() > 0)
            {
                c->process (left, 0, n);
                if (right != nullptr)
                    c->process (right, 1, n);
            }
        if (bands.getNumStages() > 0)
        {
            bands.process (left, 0, n);
            if (right != nullptr)
                bands.process (right, 1, n);
        }
    }

    double magnitudeDb (double freq) const
    {
        return hp.magnitudeDb (freq, sampleRate) + lp.magnitudeDb (freq, sampleRate)
             + bands.magnitudeDb (freq, sampleRate);
    }

private:
    static bool anyBand (const Params& p) noexcept
    {
        return p.low.on || p.lowMid.on || p.highMid.on || p.high.on;
    }

    double sampleRate = 48000.0;
    Params params;
    bool designed = false;
    BiquadCascade<4> hp, lp;  // up to 8th order
    BiquadCascade<4> bands;
};

} // namespace jdm
