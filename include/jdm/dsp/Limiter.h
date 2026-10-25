#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

namespace jdm
{

// Stereo lookahead brickwall limiter: Zenith's core (Transparent style),
// for a master-bus safety stage.
//
// Per sample: linked peak → required gain r = min (1, ceiling / peak) →
// sliding minimum of r over the last L+1 samples → exponential release (a
// rise-rate limit toward 1) → two cascaded box-car means of total support
// A ≤ L. The audio is delayed by exactly L. The smoothed envelope is a convex
// combination of values each ≤ the sliding minimum whose window contains the
// sample being output, so the output never exceeds the ceiling (proof in
// Zenith's LimiterEngine.h); the final clamp is a guard, not the mechanism.
//
// The lookahead is fixed at prepare(), so the reported latency is constant.
// Switching the limiter off (`setEnabled (false)`) keeps the delay.
class Limiter
{
public:
    void prepare (double sampleRate, float lookaheadMs = 1.0f)
    {
        fs = sampleRate;
        lookahead = juce::jmax (1, (int) std::lround (fs * 0.001 * lookaheadMs));
        box1 = juce::jmax (1, (lookahead + 1) / 2);
        box2 = juce::jmax (1, lookahead - box1 + 1);
        for (auto& s : state)
        {
            s.delay.assign ((size_t) lookahead, 0.0f);
            s.box1.assign ((size_t) box1, 1.0f);
            s.box2.assign ((size_t) box2, 1.0f);
        }
        minDeque.assign ((size_t) lookahead + 2, { 1.0f, 0 });
        ceilingSmooth.reset (sampleRate, 0.02);
        ceilingSmooth.setCurrentAndTargetValue (ceilingSmooth.getTargetValue());
        setRelease (releaseMs);
        reset();
    }

    int getLatencySamples() const noexcept { return lookahead; }

    void reset() noexcept
    {
        for (auto& s : state)
        {
            std::fill (s.delay.begin(), s.delay.end(), 0.0f);
            std::fill (s.box1.begin(), s.box1.end(), 1.0f);
            std::fill (s.box2.begin(), s.box2.end(), 1.0f);
            s.b1Sum = (double) box1;
            s.b2Sum = (double) box2;
            s.b1Pos = s.b2Pos = 0;
        }
        dPos = 0;
        dqHead = dqTail = dqCount = 0;
        sampleIndex = 0;
        release = 1.0f;
    }

    void setEnabled (bool on) noexcept { enabled = on; }
    void setCeilingDb (float db) noexcept { ceilingSmooth.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-30.0f, 0.0f, db))); }
    void setRelease (float ms) noexcept
    {
        releaseMs = juce::jlimit (5.0f, 2500.0f, ms);
        releaseCoeff = 1.0f - (float) std::exp (-1.0 / (fs * releaseMs * 0.001));
    }

    // In place; `right` may be null. Returns the block's maximum gain
    // reduction in dB (positive).
    float process (float* left, float* right, int n) noexcept
    {
        float minGain = 1.0f;
        float* ch[2] = { left, right };
        const int numCh = right != nullptr ? 2 : 1;

        for (int i = 0; i < n; ++i)
        {
            const float ceiling = ceilingSmooth.getNextValue();
            float peak = std::abs (left[i]);
            if (right != nullptr)
                peak = juce::jmax (peak, std::abs (right[i]));

            float e = 1.0f;
            if (enabled)
            {
                pushMin (peak > ceiling ? ceiling / peak : 1.0f);
                const float m = minDeque[(size_t) dqHead].value;
                if (m < release)
                    release = m;
                else
                    release += (1.0f - release) * releaseCoeff;
            }

            for (int c = 0; c < numCh; ++c)
            {
                auto& s = state[(size_t) c];
                if (enabled)
                    e = boxPush (s.box2, s.b2Pos, s.b2Sum, box2, boxPush (s.box1, s.b1Pos, s.b1Sum, box1, release));
                const float delayed = s.delay[(size_t) dPos];
                s.delay[(size_t) dPos] = ch[c][i];
                ch[c][i] = enabled ? juce::jlimit (-ceiling, ceiling, delayed * e) : delayed;
            }
            dPos = dPos + 1 >= lookahead ? 0 : dPos + 1;
            minGain = juce::jmin (minGain, e);
        }
        lastGr = -juce::Decibels::gainToDecibels (juce::jmax (minGain, 1.0e-6f), -120.0f);
        return lastGr;
    }

    float currentGainReductionDb() const noexcept { return lastGr; }

private:
    void pushMin (float value) noexcept
    {
        const int cap = (int) minDeque.size();
        while (dqCount > 0)
        {
            const int back = (dqTail + cap - 1) % cap;
            if (minDeque[(size_t) back].value < value)
                break;
            dqTail = back;
            --dqCount;
        }
        minDeque[(size_t) dqTail] = { value, sampleIndex + lookahead + 1 };
        dqTail = (dqTail + 1) % cap;
        ++dqCount;

        ++sampleIndex;
        if (minDeque[(size_t) dqHead].expires <= sampleIndex)
        {
            dqHead = (dqHead + 1) % cap;
            --dqCount;
        }
    }

    static float boxPush (std::vector<float>& buf, int& pos, double& sum, int len, float value) noexcept
    {
        sum += (double) value - (double) buf[(size_t) pos];
        buf[(size_t) pos] = value;
        pos = pos + 1 >= len ? 0 : pos + 1;
        return (float) (sum / (double) len);
    }

    struct MinEntry { float value; juce::int64 expires; };
    struct Channel
    {
        std::vector<float> delay, box1, box2;
        int b1Pos = 0, b2Pos = 0;
        double b1Sum = 0.0, b2Sum = 0.0;
    };

    double fs = 48000.0;
    int lookahead = 48, box1 = 24, box2 = 25, dPos = 0;
    std::vector<MinEntry> minDeque;
    int dqHead = 0, dqTail = 0, dqCount = 0;
    juce::int64 sampleIndex = 0;
    float release = 1.0f, releaseCoeff = 0.001f, releaseMs = 100.0f, lastGr = 0.0f;
    bool enabled = true;
    juce::SmoothedValue<float> ceilingSmooth { juce::Decibels::decibelsToGain (-0.3f) };
    std::array<Channel, 2> state;
};

} // namespace jdm
