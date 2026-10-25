#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

namespace jdm
{

// Channel/bus compressor: Meridian's feed-forward core without its styles,
// lookahead or M/S routing, i.e. what a mixer strip needs.
//
// Per sample: peak or short-RMS detection (stereo-linked by channel max,
// blended by `link`), a log-domain gain computer with a soft knee, branching
// attack/release smoothing of the gain reduction in dB (Giannoulis et al.),
// optionally Meridian's dual-envelope programme-dependent release. Makeup and
// mix (parallel blend) are smoothed per sample so block-rate updates never
// step. No latency.
class Compressor
{
public:
    struct Params
    {
        float thresholdDb = -18.0f;
        float ratio = 4.0f;
        float kneeDb = 6.0f;
        float attackMs = 10.0f;
        float releaseMs = 100.0f;
        float makeupDb = 0.0f;
        float mix = 1.0f;          // 0 … 1: dry/compressed blend (parallel)
        bool rms = false;          // detector: short RMS (else peak)
        bool autoRelease = false;  // dual-envelope release
        float link = 1.0f;         // stereo link 0 … 1

        bool operator== (const Params&) const = default;
    };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        smoothCoeff = 1.0f - std::exp (-1.0f / (float) (fs * 0.005));
        reset();
        configured = false;
        setParams (params);
    }

    void reset() noexcept
    {
        grFast = {};
        grSlow = {};
        meanSquare = {};
        appliedGr = {};
        snap = true;
    }

    void setParams (const Params& p) noexcept
    {
        if (configured && p == params)
            return;
        params = p;
        configured = true;

        ratio = juce::jlimit (1.0f, 1000.0f, p.ratio);
        knee = juce::jlimit (0.0f, 48.0f, p.kneeDb);
        const float att = juce::jmax (0.02f, p.attackMs);
        const float rel = juce::jmax (1.0f, p.releaseMs);
        attackCoeff = timeToCoeff (att);
        releaseCoeff = timeToCoeff (rel);
        slowAttackCoeff = timeToCoeff (juce::jmax (200.0f, att * 4.0f));
        slowReleaseCoeff = timeToCoeff (rel * 5.0f);
        rmsCoeff = timeToCoeff (10.0f);
        makeupTarget = juce::Decibels::decibelsToGain (p.makeupDb);
        mixTarget = juce::jlimit (0.0f, 1.0f, p.mix);
        if (snap)
        {
            snap = false;
            makeupCur = makeupTarget;
            mixCur = mixTarget;
        }
    }

    // Static curve: input level (dB) → output level (dB).
    float outputLevelDb (float inDb) const noexcept
    {
        const float over2 = 2.0f * (inDb - params.thresholdDb);
        if (over2 < -knee)
            return inDb;
        const float invR = 1.0f / ratio;
        if (over2 <= knee && knee > 0.0f)
        {
            const float x = inDb - params.thresholdDb + knee * 0.5f;
            return inDb + (invR - 1.0f) * x * x / (2.0f * knee);
        }
        return params.thresholdDb + (inDb - params.thresholdDb) * invR;
    }

    // In place. `right` may be null. `keyLeft/keyRight` (optional) replace
    // the audio as the detector input (external side chain). Returns the
    // block's maximum gain reduction in dB (positive).
    float process (float* left, float* right, int n,
                   const float* keyLeft = nullptr, const float* keyRight = nullptr) noexcept
    {
        const float* dl = keyLeft != nullptr ? keyLeft : left;
        const float* dr = keyRight != nullptr ? keyRight : right;
        float maxGr = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            makeupCur += (makeupTarget - makeupCur) * smoothCoeff;
            mixCur += (mixTarget - mixCur) * smoothCoeff;

            if (right == nullptr)
            {
                left[i] *= blend (envelope (0, detect (0, dl[i])));
                maxGr = juce::jmax (maxGr, appliedGr[0]);
                continue;
            }

            const float d0 = detect (0, dl[i]);
            const float d1 = detect (1, dr != nullptr ? dr[i] : dl[i]);
            const float m = juce::jmax (d0, d1);
            left[i] *= blend (envelope (0, d0 + params.link * (m - d0)));
            right[i] *= blend (envelope (1, d1 + params.link * (m - d1)));
            maxGr = juce::jmax (maxGr, juce::jmax (appliedGr[0], appliedGr[1]));
        }
        return maxGr;
    }

    float currentGainReductionDb() const noexcept { return juce::jmax (appliedGr[0], appliedGr[1]); }

private:
    float detect (int c, float x) noexcept
    {
        if (! params.rms)
            return std::abs (x);
        auto& ms = meanSquare[(size_t) c];
        ms = rmsCoeff * ms + (1.0f - rmsCoeff) * x * x;
        return std::sqrt (ms);
    }

    // Returns the wet gain (makeup included).
    float envelope (int c, float det) noexcept
    {
        const auto i = (size_t) c;
        const float detDb = juce::Decibels::gainToDecibels (det, -120.0f);
        const float target = detDb - outputLevelDb (detDb);

        auto& f = grFast[i];
        f = (target > f ? attackCoeff : releaseCoeff) * (f - target) + target;

        float gr = f;
        if (params.autoRelease)
        {
            auto& s = grSlow[i];
            s = (target > s ? slowAttackCoeff : slowReleaseCoeff) * (s - target) + target;
            gr = 0.5f * (f + s);
        }
        appliedGr[i] = gr;
        return juce::Decibels::decibelsToGain (-gr) * makeupCur;
    }

    float blend (float wet) const noexcept { return (1.0f - mixCur) + mixCur * wet; }

    float timeToCoeff (float ms) const noexcept
    {
        return std::exp (-1.0f / (float) (fs * 0.001 * (double) ms));
    }

    Params params;
    bool configured = false, snap = true;
    double fs = 48000.0;
    float ratio = 4.0f, knee = 6.0f;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f, slowAttackCoeff = 0.0f, slowReleaseCoeff = 0.0f;
    float rmsCoeff = 0.0f, smoothCoeff = 1.0f;
    float makeupTarget = 1.0f, makeupCur = 1.0f, mixTarget = 1.0f, mixCur = 1.0f;
    std::array<float, 2> grFast {}, grSlow {}, meanSquare {}, appliedGr {};
};

} // namespace jdm
