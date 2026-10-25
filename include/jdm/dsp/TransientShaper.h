#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace jdm
{

// Level-independent transient shaper (the envelope-difference design).
//
// Three peak followers on the linked input:
//   fast   attack 0.5 ms, release 50 ms
//   slow   attack 15 ms,  release 50 ms  → fast − slow (dB) > 0 on a transient
//   long   attack 0.5 ms, release 300 ms → long − fast (dB) > 0 in the tail
// The gain in dB is attack × transient + sustain × tail, each scaled so ±1 is
// a strong setting, clamped to ±maxDb and lightly smoothed. Because both
// terms are ratios of envelopes, the effect does not depend on the input
// level (no threshold to set).
class TransientShaper
{
public:
    struct Params
    {
        float attack = 0.0f;   // -1 … +1
        float sustain = 0.0f;  // -1 … +1

        bool operator== (const Params&) const = default;
    };

    static constexpr float maxDb = 18.0f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        auto coeff = [this] (double ms) { return (float) std::exp (-1.0 / (fs * 0.001 * ms)); };
        fastAtt = coeff (0.5);
        fastRel = coeff (50.0);
        slowAtt = coeff (15.0);
        longRel = coeff (300.0);
        smooth = coeff (1.0);
        reset();
    }

    void reset() noexcept
    {
        fast = slow = longEnv = 0.0f;
        gainDb = 0.0f;
    }

    void setParams (const Params& p) noexcept { params = p; }

    bool isActive() const noexcept
    {
        return std::abs (params.attack) > 1.0e-4f || std::abs (params.sustain) > 1.0e-4f;
    }

    void process (float* left, float* right, int n) noexcept
    {
        constexpr float tiny = 1.0e-6f;
        for (int i = 0; i < n; ++i)
        {
            float x = std::abs (left[i]);
            if (right != nullptr)
                x = juce::jmax (x, std::abs (right[i]));

            fast = follow (fast, x, fastAtt, fastRel);
            slow = follow (slow, x, slowAtt, fastRel);
            longEnv = follow (longEnv, x, fastAtt, longRel);

            const float lf = std::log10 (fast + tiny);
            const float transient = juce::jmax (0.0f, 20.0f * (lf - std::log10 (slow + tiny)));
            const float tail = juce::jmax (0.0f, 20.0f * (std::log10 (longEnv + tiny) - lf));
            const float target = juce::jlimit (-maxDb, maxDb, params.attack * transient + params.sustain * tail);
            gainDb = target + (gainDb - target) * smooth;

            const float g = juce::Decibels::decibelsToGain (gainDb);
            left[i] *= g;
            if (right != nullptr)
                right[i] *= g;
        }
    }

private:
    static float follow (float e, float x, float att, float rel) noexcept
    {
        return x > e ? x + (e - x) * att : x + (e - x) * rel;
    }

    Params params;
    double fs = 48000.0;
    float fastAtt = 0.0f, fastRel = 0.0f, slowAtt = 0.0f, longRel = 0.0f, smooth = 0.0f;
    float fast = 0.0f, slow = 0.0f, longEnv = 0.0f, gainDb = 0.0f;
};

} // namespace jdm
