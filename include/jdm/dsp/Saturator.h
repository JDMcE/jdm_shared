#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace jdm
{

// Soft saturation: y = tanh (g·x + b) − tanh (b), divided by its slope at 0
// (g·sech² b), then a DC blocker. The normalisation keeps quiet material at
// unity, so `drive` decides how hard the peaks are rounded off rather than how
// loud the channel gets; the bias `b` (from `warmth`, applied after the drive
// so it means the same at every drive) makes the curve asymmetric, adding
// even harmonics. Blended with the dry signal by `mix`. Not oversampled: at the
// drive range a mixer strip offers, the aliasing sits well below the content.
class Saturator
{
public:
    struct Params
    {
        float driveDb = 0.0f;   // 0 … 24
        float warmth = 0.0f;    // 0 … 1
        float mix = 1.0f;       // 0 … 1

        bool operator== (const Params&) const = default;
    };

    void prepare (double sampleRate)
    {
        dcCoeff = (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sampleRate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : dc)
            s = {};
    }

    void setParams (const Params& p) noexcept
    {
        params = p;
        g = juce::Decibels::decibelsToGain (juce::jlimit (0.0f, 36.0f, p.driveDb));
        bias = 0.5f * juce::jlimit (0.0f, 1.0f, p.warmth);
        offset = std::tanh (bias);
        norm = 1.0f / (g * (1.0f - offset * offset));
        mix = juce::jlimit (0.0f, 1.0f, p.mix);
    }

    bool isActive() const noexcept { return params.driveDb > 1.0e-3f && params.mix > 0.0f; }

    void process (float* left, float* right, int n) noexcept
    {
        processChannel (left, n, dc[0]);
        if (right != nullptr)
            processChannel (right, n, dc[1]);
    }

private:
    struct DcState { float x1 = 0.0f, y1 = 0.0f; };

    void processChannel (float* x, int n, DcState& s) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            float y = (std::tanh (g * x[i] + bias) - offset) * norm;
            if (bias > 0.0f)
            {
                const float out = y - s.x1 + dcCoeff * s.y1;
                s.x1 = y;
                s.y1 = out;
                y = out;
            }
            x[i] += mix * (y - x[i]);
        }
    }

    Params params;
    float g = 1.0f, bias = 0.0f, offset = 0.0f, norm = 1.0f, mix = 1.0f, dcCoeff = 0.999f;
    DcState dc[2];
};

} // namespace jdm
